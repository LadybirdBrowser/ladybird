/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::container_conditions::{ContainerConditionData, ContainerConditionsData};
use crate::css::counter_style::{CounterStyleData, FfiCounterStyle};
use crate::css::css_enums::{keyword_from_ascii_case_insensitive, keyword_to_generic_font_family};
use crate::css::css_string::CssString;
use crate::css::css_tokenizer::{
    CssNumberType, ParserString, ParserToken, ParserTokenKind, SourcePosition, TokenizerInput, tokenize_for_parser,
};
use crate::css::declaration_block::{DeclarationBlock, DeclarationBlockData, DeclaredProperty};
use crate::css::descriptor_block::{DescriptorBlockData, DescriptorData, FfiDescriptorBlock};
use crate::css::descriptor_metadata::{CUSTOM_DESCRIPTOR_ID, descriptor_longhands};
use crate::css::ffi_support::FfiUtf16View;
pub use crate::css::font_feature_values::FontFeatureValuesRuleKind;
use crate::css::font_feature_values::{FontFeatureValuesData, FontFeatureValuesRule};
use crate::css::function_signature::{FunctionParameterData, FunctionSignature};
use crate::css::import_rule::ImportRuleData;
use crate::css::keyframes::{FfiKeyframe, KeyframeData};
use crate::css::layer_names::LayerNames;
use crate::css::media_list::{MediaList, MediaListData};
use crate::css::namespace_rule::NamespaceRuleData;
use crate::css::parser::component_value::{
    ComponentKind, ComponentSerializationMode, ComponentValue, consume_a_component_value,
    consume_a_list_of_component_values, trim_whitespace,
};
use crate::css::parser::descriptor_parser::parse_descriptor;
use crate::css::parser::query_parser::{
    ContainerCondition, Expression, FfiQueryHandle, MediaQuery, QueryKind, declared_namespaces_from_context,
    expression_query_handle, media_query_handle, parse_container_condition_list_from_component_values,
    parse_media_query_list_from_component_values, parse_supports_condition_from_component_values,
    parse_supports_declaration_from_component_values, resolve_query_feature, supports_feature_matches,
};
use crate::css::parser::stylesheet_cache::{CachedParseMetadata, parse_with_cache};
use crate::css::parser::syntax::{SyntaxNode, parse_syntax, parse_with_syntax};
use crate::css::parser::value_parser::{
    FfiValueParsingContext, FfiValueParsingContextKind, ParseContext, ParseOutcome, is_valid_custom_ident,
    parse_css_value_with_utf16_source, parse_url_value,
};
use crate::css::property_metadata::{property_id, property_id_from_name};
use crate::css::property_rule::PropertyRuleData;
use crate::css::rule::{NativeRule, NativeRuleList, NativeRuleType, RulePayload};
use crate::css::scope_selectors::ScopeSelectors;
use crate::css::selector_parser::{
    RustParsedSelectorList, SelectorType, StyleNestingParent, parse_selector_list_from_component_values,
};
use crate::css::serialize::serialize_component_values_to_utf16;
use crate::css::style_compute::value_is_computationally_independent;
use crate::css::style_rule::StyleRule;
use crate::css::style_value::{RetainedRequestUrlModifierList, RetainedString, StyleValueData};
use std::borrow::Cow;
use std::cell::RefCell;
use std::ffi::c_void;
use std::fmt;
use std::rc::Rc;
use std::sync::Arc;

mod shared_rules;

#[cfg(test)]
mod ownership_tests;
pub(crate) use shared_rules::{ParsedRuleList, SharedRule};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
#[allow(dead_code)] // Most contexts are constructed through the C++ FFI.
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
enum PagePseudoClass {
    Left = 0,
    Right = 1,
    First = 2,
    Blank = 3,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ParsedRuleKind {
    Qualified,
    Invalid,
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
    nesting_parent: StyleNestingParent,
    valid_in_context: bool,
    outer_rule_name: Option<ParserString>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct QualifiedRule {
    pub prelude: Vec<ComponentValue>,
    pub prelude_is_selector: bool,
    nesting_parent: StyleNestingParent,
    valid_in_context: bool,
    outer_rule_name: Option<ParserString>,
    pub declarations: Vec<Declaration>,
    pub children: Vec<RuleOrDeclarations>,
    pub source_position: Option<SourcePosition>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Declaration {
    pub name: ParserString,
    pub value: Vec<ComponentValue>,
    pub important: bool,
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
        start: Option<RustParsedSelectorList>,
        end: Option<RustParsedSelectorList>,
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
    FontFeatureValuesRule(FontFeatureValuesRuleKind),
}

#[derive(Clone, Debug, PartialEq)]
struct ParsedPageSelector {
    name: Option<ParserString>,
    pseudo_classes: Vec<PagePseudoClass>,
}

pub struct PageSelectorList {
    selectors: Box<[ParsedPageSelector]>,
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
                PagePseudoClass::Left
            } else if equals_ascii_case_insensitive(pseudo_class, b"right") {
                PagePseudoClass::Right
            } else if equals_ascii_case_insensitive(pseudo_class, b"first") {
                PagePseudoClass::First
            } else if equals_ascii_case_insensitive(pseudo_class, b"blank") {
                PagePseudoClass::Blank
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

// https://drafts.csswg.org/cssom/#parse-a-list-of-css-page-selectors
fn parse_page_selector_list<'a>(source: impl Into<TokenizerInput<'a>>) -> Option<Vec<ParsedPageSelector>> {
    // NB: This replaces the C++ path that wrapped the source in `@page <source> {}` and parsed
    //     exactly one rule. Consuming the complete component-value list
    //     preserves rejection of trailing tokens without constructing synthetic CSS.
    let values = consume_a_list_of_component_values(tokenize_for_parser(source)).ok()?;
    let ParsedRulePrelude::PageSelectors(selectors) = parse_page_selectors(&values) else {
        return None;
    };
    Some(selectors)
}

impl PageSelectorList {
    fn new(parsed_selectors: Vec<ParsedPageSelector>) -> Self {
        Self {
            selectors: parsed_selectors.into_boxed_slice(),
        }
    }
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

fn parse_scope_prelude(
    values: &[ComponentValue],
    declared_namespaces: &[TokenizerInput<'_>],
    nesting_parent: StyleNestingParent,
) -> ParsedRulePrelude {
    let Some((start, end)) = scope_selector_components(values) else {
        return ParsedRulePrelude::Invalid;
    };
    let parse_selector = |values: &[ComponentValue], selector_type| {
        let selectors = parse_selector_list_from_component_values(values, declared_namespaces, selector_type).ok()?;
        if selectors.is_empty() || selectors.contains_pseudo_element() {
            return None;
        }
        Some(selectors)
    };
    let start = match start {
        Some(start) => {
            let selector_type = if nesting_parent != StyleNestingParent::None {
                SelectorType::Relative
            } else {
                SelectorType::Standalone
            };
            let Some(start) = parse_selector(start, selector_type) else {
                return ParsedRulePrelude::Invalid;
            };
            Some(start.adapt_for_nesting(nesting_parent))
        }
        None => None,
    };
    let end = match end {
        Some(end) => {
            let Some(end) = parse_selector(end, SelectorType::Relative) else {
                return ParsedRulePrelude::Invalid;
            };
            Some(end)
        }
        None => None,
    };
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

fn parse_import_selector(
    values: &[ComponentValue],
    declared_namespaces: &[TokenizerInput<'_>],
    selector_type: SelectorType,
) -> Option<RustParsedSelectorList> {
    let selectors = parse_selector_list_from_component_values(values, declared_namespaces, selector_type).ok()?;
    if selectors.is_empty() || selectors.contains_pseudo_element() {
        return None;
    }
    Some(selectors)
}

fn parse_import_url(context: &ParseContext, value: &ComponentValue) -> Option<ParsedStyleValue> {
    let url = match &value.kind {
        ComponentKind::Token(ParserTokenKind::String(url)) => StyleValueData::Url {
            url: RetainedString::from_utf16(url)?,
            url_type: 0,
            modifiers: RetainedRequestUrlModifierList::from_retained_modifiers(Vec::new()),
        },
        ComponentKind::Token(ParserTokenKind::Url(_)) => parse_url_value(context, value)?,
        ComponentKind::Function { name, .. } if equals_ascii_case_insensitive(name, b"url") => {
            parse_url_value(context, value)?
        }
        _ => return None,
    };
    Some(ParsedStyleValue(Arc::new(url)))
}

fn parse_import_prelude<R>(
    values: &[ComponentValue],
    context: &ParseContext,
    declared_namespaces: &[TokenizerInput<'_>],
    resolve_feature: &R,
) -> ParsedRulePrelude
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let mut position = 0;
    skip_whitespace(values, &mut position);
    let Some(url) = values.get(position) else {
        return ParsedRulePrelude::Invalid;
    };
    let Some(url) = parse_import_url(context, url) else {
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
                let Some(selectors) = parse_import_selector(start, declared_namespaces, SelectorType::Standalone)
                else {
                    return ParsedRulePrelude::Invalid;
                };
                scope_start = Some(selectors);
            }
            if let Some(end) = end {
                let Some(selectors) = parse_import_selector(end, declared_namespaces, SelectorType::Relative) else {
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
            let Some(expression) = parse_supports_condition_from_component_values(contents, &|kind, value| {
                supports_feature_matches(context, declared_namespaces, kind, value)
            })
            .or_else(|| {
                parse_supports_declaration_from_component_values(contents, &|kind, value| {
                    supports_feature_matches(context, declared_namespaces, kind, value)
                })
            }) else {
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
    rule_kind: ParsedRuleKind,
    parse_context: Option<&ParseContext>,
    declared_namespaces: &[TokenizerInput<'_>],
    resolve_feature: &R,
) -> ParsedRulePrelude
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    match (rule, rule_kind) {
        (Rule::Qualified(rule), ParsedRuleKind::Qualified) if !rule.prelude_is_selector => {
            parse_keyframe_selectors(&rule.prelude)
        }
        (Rule::Qualified(_), ParsedRuleKind::Qualified) => ParsedRulePrelude::Unparsed,
        (Rule::At(rule), ParsedRuleKind::Layer) => parse_layer_prelude(&rule.prelude, rule.has_block),
        (Rule::At(rule), ParsedRuleKind::Property) => parse_context
            .map(|context| parse_property_prelude(rule, context))
            .unwrap_or_else(|| {
                parse_single_name(&rule.prelude, |name| {
                    name.starts_with(&[u16::from(b'-'), u16::from(b'-')]) && name.len() > 2
                })
            }),
        (Rule::At(rule), ParsedRuleKind::CounterStyle) => parse_single_name(&rule.prelude, |name| {
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
        (Rule::At(rule), ParsedRuleKind::Keyframes) => {
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
        (Rule::At(rule), ParsedRuleKind::Namespace) => parse_namespace_prelude(&rule.prelude),
        (Rule::At(rule), ParsedRuleKind::Page) => parse_page_selectors(&rule.prelude),
        (Rule::At(rule), ParsedRuleKind::FontFeatureValues) => parse_font_family_names(&rule.prelude),
        (Rule::At(rule), ParsedRuleKind::FontFeatureValuesRule) => {
            let (kind, _) = font_feature_values_rule(rule.name.as_ref()).unwrap();
            ParsedRulePrelude::FontFeatureValuesRule(kind)
        }
        (Rule::At(rule), ParsedRuleKind::Scope) => {
            parse_scope_prelude(&rule.prelude, declared_namespaces, rule.nesting_parent)
        }
        (Rule::At(rule), ParsedRuleKind::Import) => parse_context
            .map(|context| parse_import_prelude(&rule.prelude, context, declared_namespaces, resolve_feature))
            .unwrap_or(ParsedRulePrelude::Invalid),
        (Rule::At(rule), ParsedRuleKind::Function) => parse_context
            .map(|context| parse_function_prelude(rule, context))
            .unwrap_or(ParsedRulePrelude::Invalid),
        (Rule::At(rule), ParsedRuleKind::Media) => ParsedRulePrelude::MediaQueries(
            parse_media_query_list_from_component_values(&rule.prelude, resolve_feature),
        ),
        (Rule::At(rule), ParsedRuleKind::Supports) => {
            parse_supports_condition_from_component_values(&rule.prelude, &|kind, value| {
                parse_context.is_some_and(|context| supports_feature_matches(context, declared_namespaces, kind, value))
            })
            .map(ParsedRulePrelude::SupportsCondition)
            .unwrap_or(ParsedRulePrelude::Invalid)
        }
        (Rule::At(rule), ParsedRuleKind::Container) => {
            parse_container_condition_list_from_component_values(&rule.prelude, resolve_feature)
                .map(ParsedRulePrelude::ContainerConditions)
                .unwrap_or(ParsedRulePrelude::Invalid)
        }
        (Rule::At(rule), ParsedRuleKind::FontFace | ParsedRuleKind::Margin) => {
            if non_whitespace(&rule.prelude).is_empty() {
                ParsedRulePrelude::Empty
            } else {
                ParsedRulePrelude::Invalid
            }
        }
        (Rule::At(_), ParsedRuleKind::Unknown | ParsedRuleKind::IgnoredVendor) => ParsedRulePrelude::Unparsed,
        (_, kind) => unreachable!("no prelude grammar for rule kind {kind:?}"),
    }
}

struct Parser {
    tokens: Vec<ParserToken>,
    position: usize,
    rule_context: Vec<RuleContext>,
    rule_names: Vec<Option<ParserString>>,
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

fn utf16_from_ascii(value: &[u8]) -> ParserString {
    value
        .iter()
        .map(|&code_unit| u16::from(code_unit))
        .collect::<Vec<_>>()
        .into_boxed_slice()
        .into()
}

fn context_rule_name(context: RuleContext) -> Option<ParserString> {
    let name = match context {
        RuleContext::Unknown | RuleContext::SupportsCondition => return None,
        RuleContext::Style => b"style".as_slice(),
        RuleContext::AtContainer => b"@container",
        RuleContext::AtCounterStyle => b"@counter-style",
        RuleContext::AtMedia => b"@media",
        RuleContext::AtFontFace => b"@font-face",
        RuleContext::AtFontFeatureValues => b"@font-feature-values",
        RuleContext::FontFeatureValue => b"@font-feature-value",
        RuleContext::AtFunction => b"@function",
        RuleContext::AtKeyframes => b"@keyframes",
        RuleContext::Keyframe => b"keyframe",
        RuleContext::AtSupports => b"@supports",
        RuleContext::AtScope => b"@scope",
        RuleContext::AtLayer => b"@layer",
        RuleContext::AtProperty => b"@property",
        RuleContext::AtPage => b"@page",
        RuleContext::Margin => b"@margin",
    };
    Some(utf16_from_ascii(name))
}

fn at_rule_name(name: &[u16]) -> ParserString {
    let mut result = Vec::with_capacity(name.len() + 1);
    result.push(u16::from(b'@'));
    result.extend_from_slice(name);
    result.into_boxed_slice().into()
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

fn rule_kind(rule: &Rule) -> ParsedRuleKind {
    let Rule::At(rule) = rule else {
        return ParsedRuleKind::Qualified;
    };
    at_rule_kind(rule.name.as_ref())
}

fn at_rule_kind(name: &[u16]) -> ParsedRuleKind {
    if equals_ascii_case_insensitive(name, b"keyframes") || equals_ascii_case_insensitive(name, b"-webkit-keyframes") {
        ParsedRuleKind::Keyframes
    } else if has_ignored_vendor_prefix(name.into()) {
        ParsedRuleKind::IgnoredVendor
    } else if equals_ascii_case_insensitive(name, b"container") {
        ParsedRuleKind::Container
    } else if equals_ascii_case_insensitive(name, b"counter-style") {
        ParsedRuleKind::CounterStyle
    } else if equals_ascii_case_insensitive(name, b"font-face") {
        ParsedRuleKind::FontFace
    } else if equals_ascii_case_insensitive(name, b"font-feature-values") {
        ParsedRuleKind::FontFeatureValues
    } else if font_feature_values_rule(name).is_some() {
        ParsedRuleKind::FontFeatureValuesRule
    } else if equals_ascii_case_insensitive(name, b"function") {
        ParsedRuleKind::Function
    } else if equals_ascii_case_insensitive(name, b"import") {
        ParsedRuleKind::Import
    } else if equals_ascii_case_insensitive(name, b"layer") {
        ParsedRuleKind::Layer
    } else if is_margin_rule_name(name) {
        ParsedRuleKind::Margin
    } else if equals_ascii_case_insensitive(name, b"media") {
        ParsedRuleKind::Media
    } else if equals_ascii_case_insensitive(name, b"namespace") {
        ParsedRuleKind::Namespace
    } else if equals_ascii_case_insensitive(name, b"page") {
        ParsedRuleKind::Page
    } else if equals_ascii_case_insensitive(name, b"property") {
        ParsedRuleKind::Property
    } else if equals_ascii_case_insensitive(name, b"scope") {
        ParsedRuleKind::Scope
    } else if equals_ascii_case_insensitive(name, b"supports") {
        ParsedRuleKind::Supports
    } else {
        ParsedRuleKind::Unknown
    }
}

// NB: Mirrors the at-rule support list that this series removed from
//     Libraries/LibWeb/CSS/Parser/RustQueryParsing.cpp.
pub(crate) fn at_rule_is_supported(name: &[u16]) -> bool {
    !matches!(
        at_rule_kind(name),
        ParsedRuleKind::Qualified | ParsedRuleKind::Invalid | ParsedRuleKind::Unknown | ParsedRuleKind::IgnoredVendor
    )
}

fn font_feature_values_rule(name: &[u16]) -> Option<(FontFeatureValuesRuleKind, usize)> {
    if equals_ascii_case_insensitive(name, b"annotation") {
        Some((FontFeatureValuesRuleKind::Annotation, 1))
    } else if equals_ascii_case_insensitive(name, b"character-variant") {
        Some((FontFeatureValuesRuleKind::CharacterVariant, 2))
    } else if equals_ascii_case_insensitive(name, b"historical-forms") {
        Some((FontFeatureValuesRuleKind::HistoricalForms, 1))
    } else if equals_ascii_case_insensitive(name, b"ornaments") {
        Some((FontFeatureValuesRuleKind::Ornaments, 1))
    } else if equals_ascii_case_insensitive(name, b"styleset") {
        Some((FontFeatureValuesRuleKind::Styleset, usize::MAX))
    } else if equals_ascii_case_insensitive(name, b"stylistic") {
        Some((FontFeatureValuesRuleKind::Stylistic, 1))
    } else if equals_ascii_case_insensitive(name, b"swash") {
        Some((FontFeatureValuesRuleKind::Swash, 1))
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
    fn style_nesting_parent(&self) -> StyleNestingParent {
        self.rule_context
            .iter()
            .rev()
            .find_map(|context| match context {
                RuleContext::Style => Some(StyleNestingParent::Style),
                RuleContext::AtScope => Some(StyleNestingParent::Scope),
                _ => None,
            })
            .unwrap_or(StyleNestingParent::None)
    }

    fn new(tokens: Vec<ParserToken>, rule_context: Vec<RuleContext>) -> Self {
        let rule_names = rule_context.iter().copied().map(context_rule_name).collect();
        Self {
            tokens,
            position: 0,
            rule_context,
            rule_names,
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

    fn declaration_can_take_tokens(&self) -> bool {
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
            nesting_parent: self.style_nesting_parent(),
            valid_in_context: false,
            outer_rule_name: self.rule_names.last().cloned().flatten(),
        };
        loop {
            match self.next_kind() {
                None | Some(ParserTokenKind::Semicolon) => {
                    self.position += usize::from(self.next_kind().is_some());
                    rule.valid_in_context = self.at_rule_is_valid(&rule);
                    return Some(rule);
                }
                Some(ParserTokenKind::CloseCurly) if nested == Nested::Yes => {
                    rule.valid_in_context = self.at_rule_is_valid(&rule);
                    return Some(rule);
                }
                Some(ParserTokenKind::OpenCurly) => {
                    let context = context_for_at_rule(rule.name.as_ref());
                    self.rule_context.push(context);
                    self.rule_names.push(Some(at_rule_name(rule.name.as_ref())));
                    rule.children = self.consume_block();
                    self.rule_names.pop();
                    self.rule_context.pop();
                    rule.has_block = true;
                    rule.valid_in_context = self.at_rule_is_valid(&rule);
                    return Some(rule);
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
            nesting_parent: self.style_nesting_parent(),
            valid_in_context: false,
            outer_rule_name: self.rule_names.last().cloned().flatten(),
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
                    self.rule_names.push(context_rule_name(qualified_context));
                    rule.children = self.consume_block();
                    self.rule_names.pop();
                    self.rule_context.pop();
                    if matches!(rule.children.first(), Some(RuleOrDeclarations::Declarations(_))) {
                        let RuleOrDeclarations::Declarations(declarations) = rule.children.remove(0) else {
                            unreachable!();
                        };
                        rule.declarations = declarations;
                    }
                    rule.valid_in_context = self.qualified_rule_is_valid();
                    return Ok(rule);
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
                    if could_be_declaration && let Some(declaration) = self.consume_declaration(Nested::Yes) {
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
                        Err(QualifiedRuleResult::Invalid) => {
                            self.position += usize::from(matches!(self.next_kind(), Some(ParserTokenKind::Semicolon)));
                        }
                    }
                }
            }
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-declaration
    fn consume_declaration(&mut self, nested: Nested) -> Option<Declaration> {
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
        let can_take_tokens = self.declaration_can_take_tokens();
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
        if !self.declaration_is_valid(name.as_ref()) {
            return None;
        }
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
            Some(RuleContext::Style | RuleContext::Keyframe | RuleContext::AtScope | RuleContext::Margin) => true,
            Some(RuleContext::AtContainer | RuleContext::AtLayer | RuleContext::AtMedia | RuleContext::AtSupports) => {
                self.rule_context.contains(&RuleContext::Style)
            }
            _ => false,
        }
    }

    fn at_rule_is_valid(&self, rule: &AtRule) -> bool {
        let name = rule.name.as_ref();
        // NB: Margin and font feature value rules are only meaningful inside @page and
        //     @font-feature-values. The C++ converter dropped them anywhere else, so they must not
        //     count as valid rules for the top-level @import and @namespace ordering windows.
        if self.rule_context.is_empty() {
            return !is_margin_rule_name(name) && !is_font_feature_value_rule_name(name);
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
                !equals_ascii_case_insensitive(name, b"import")
                    && !equals_ascii_case_insensitive(name, b"namespace")
                    && !is_font_feature_value_rule_name(name)
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

pub(crate) fn supports_declaration_matches(context: &ParseContext, source: &[u16]) -> bool {
    let (mut parser, _) = Parser::from_source(source, vec![RuleContext::SupportsCondition]);
    let items = parser.consume_block_contents();
    let [RuleOrDeclarations::Declarations(declarations)] = items.as_slice() else {
        return false;
    };
    let [declaration] = declarations.as_slice() else {
        return false;
    };
    let Some(property_id) = property_id_from_name(declaration.name.as_ref()) else {
        return false;
    };

    let source = component_list_source(&declaration.value);
    let property_context = FfiValueParsingContext {
        kind: FfiValueParsingContextKind::Property,
        value: property_id,
        secondary_value: 0,
        name: Default::default(),
    };
    let mut value_context = *context;
    value_context.is_ua_style_sheet = false;
    value_context.value_contexts = &raw const property_context;
    value_context.value_context_count = 1;
    value_context.length_resolution_context = std::ptr::null();
    let outcome = parse_css_value_with_utf16_source(&value_context, property_id, &declaration.value, source.as_ref());
    if let Some(random_function_index) = unsafe { context.random_function_index.as_mut() } {
        *random_function_index = 0;
    }
    matches!(outcome, ParseOutcome::Parsed(_))
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSyntaxDeclaration {
    pub name_offset: usize,
    pub name_length: usize,
    pub value_source_offset: usize,
    pub value_source_length: usize,
    pub is_property: bool,
    pub important: bool,
    pub start_line: usize,
    pub start_column: usize,
    pub preserve_source_text: bool,
    pub property_id: u16,
    pub descriptor_id: u8,
    pub rejection: FfiDeclarationRejection,
    pub parsed_value: *const c_void,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiDeclarationRejection {
    None,
    UnknownProperty,
    IgnoredVendorPrefix,
    InvalidValue,
}

struct ParsedDeclarationList {
    local_identity: u64,
    start: usize,
    count: usize,
    declaration_block: Option<Arc<DeclarationBlockData>>,
    descriptor_block: Option<Arc<DescriptorBlockData>>,
}

enum ParsedRuleChild {
    Rule(Arc<ParsedRule>),
    Declarations(ParsedDeclarationList),
}

fn declaration_block_from_blocks<'a>(
    blocks: impl Iterator<Item = &'a Arc<DeclarationBlockData>>,
) -> Arc<DeclarationBlockData> {
    // https://drafts.csswg.org/cssom/#parse-a-css-declaration-block
    // 2. Let parsed declarations be a new empty list.
    let mut declarations: Option<Arc<DeclarationBlockData>> = None;
    // 3. For each item declaration in declarations, follow these substeps:
    for block in blocks {
        // 1. Let parsed declaration be the result of parsing declaration according to the appropriate CSS
        //    specifications, dropping parts that are said to be ignored. If the whole declaration is dropped, let
        //    parsed declaration be null.
        // 2. If parsed declaration is not null, append it to parsed declarations.
        // NB: Each declaration-list item already owns its parsed, normalized properties.
        if let Some(declarations) = &mut declarations {
            Arc::make_mut(declarations).append_block(block);
        } else {
            declarations = Some(block.clone());
        }
    }
    // 4. Return parsed declarations.
    declarations.unwrap_or_default()
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub enum FfiSyntaxDiagnosticCode {
    BadString,
    BadUrl,
    UnknownRule,
    StyleSelectorsInvalid,
    StyleEmptySelector,
    LayerInvalidName,
    LayerContainsInvalidName,
    KeyframesMustBeBlock,
    KeyframesInvalidName,
    NamespaceMustBeStatement,
    NamespaceInvalidPrelude,
    MediaExpectedBlock,
    SupportsMustBeBlock,
    SupportsClauseInvalid,
    ContainerMustBeBlock,
    ContainerConditionsInvalid,
    CounterStyleMustBeBlock,
    CounterStyleMissingName,
    FontFaceMustBeBlock,
    FontFacePreludeNotAllowed,
    FontFeatureValuesMustBeBlock,
    FunctionMustBeBlock,
    PageMustBeBlock,
    MarginMustBeBlock,
    MarginPreludeNotAllowed,
    InvalidRuleLocation,
    ImportInvalid,
    KeyframeSelectorsInvalid,
    FontFeatureValuesPreludeInvalid,
    FunctionPreludeInvalid,
    PagePreludeInvalid,
    PropertyPreludeInvalid,
    ScopeInvalid,
    MisplacedImport,
    MisplacedNamespace,
    InvalidRuleContext,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSyntaxDiagnostic {
    pub code: FfiSyntaxDiagnosticCode,
    pub start_line: usize,
    pub start_column: usize,
    pub end_line: usize,
    pub end_column: usize,
    pub primary_offset: usize,
    pub primary_length: usize,
    pub secondary_offset: usize,
    pub secondary_length: usize,
    pub prelude_offset: usize,
    pub prelude_length: usize,
}

struct ParsedDeclaration {
    name_offset: usize,
    name_length: usize,
    value_source_offset: usize,
    value_source_length: usize,
    is_property: bool,
    important: bool,
    start_line: usize,
    start_column: usize,
    preserve_source_text: bool,
    property_id: u16,
    descriptor_id: u8,
    rejection: FfiDeclarationRejection,
    parsed_value: Option<Arc<StyleValueData>>,
    font_feature_values_start: usize,
    font_feature_value_count: usize,
}

struct ParsedRule {
    local_identity: u64,
    is_qualified: bool,
    rule_kind: ParsedRuleKind,
    name_offset: usize,
    name_length: usize,
    declaration_block: Option<Arc<DeclarationBlockData>>,
    children: Box<[ParsedRuleChild]>,
    native_children: Box<[usize]>,
    has_block: bool,
    has_source_position: bool,
    start_line: usize,
    start_column: usize,
    page_selector_list: Option<Arc<PageSelectorList>>,
    import_rule: Option<Arc<ImportRuleData>>,
    supports_condition: Option<Arc<FfiQueryHandle>>,
    selector_list: Option<Arc<RustParsedSelectorList>>,
    descriptor_block: Option<Arc<DescriptorBlockData>>,
    layer_names: Option<Arc<LayerNames>>,
    container_conditions: Option<Arc<ContainerConditionsData>>,
    font_feature_values: Option<Arc<FontFeatureValuesData>>,
    keyframe: Option<Arc<KeyframeData>>,
    keyframes_name: Option<CssString>,
    scope_selectors: Option<Arc<ScopeSelectors>>,
    media_list: Option<Arc<MediaListData>>,
    counter_style: Option<Arc<CounterStyleData>>,
    namespace_rule: Option<Arc<NamespaceRuleData>>,
    property_rule: Option<Arc<PropertyRuleData>>,
    function_signature: Option<Arc<FunctionSignature>>,
}

// Parser state is discarded before publishing the owned result.
struct SyntaxParseBuilder {
    declarations_kind: NestedDeclarationsKind,
    values: Vec<u16>,
    declarations: Vec<ParsedDeclaration>,
    rules: Vec<Arc<ParsedRule>>,
    items: Vec<ParsedRuleChild>,
    identity_count: u64,
    font_feature_values: Vec<u32>,
    diagnostics: Vec<FfiSyntaxDiagnostic>,
    declared_namespaces: Vec<ParserString>,
    parse_context: *const ParseContext,
    preserve_property_source_text: bool,
}

// The shared graph owns typed Rust payloads, independently of any FFI views.
pub struct ParsedStyleSheet {
    identity_count: u64,
    pub(super) cache_metadata: Option<CachedParseMetadata>,
    declarations_kind: NestedDeclarationsKind,
    values: Box<[u16]>,
    declarations: Box<[ParsedDeclaration]>,
    rules: Box<[Arc<ParsedRule>]>,
    items: Box<[ParsedRuleChild]>,
    native_roots: Box<[usize]>,
    diagnostics: Box<[FfiSyntaxDiagnostic]>,
    declaration_errors: Box<[usize]>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<ParsedStyleSheet>();
};

#[derive(Clone, Copy)]
enum NestedDeclarationsKind {
    Style,
    Function,
}

impl ParsedStyleSheet {
    pub(crate) fn identity_count(&self) -> u64 {
        self.identity_count
    }

    fn collect_declaration_errors(&self) -> Box<[usize]> {
        fn visit_item(sheet: &ParsedStyleSheet, item: &ParsedRuleChild, errors: &mut Vec<usize>) {
            match item {
                ParsedRuleChild::Declarations(list) => {
                    for index in list.start..list.start + list.count {
                        if matches!(
                            sheet.declarations[index].rejection,
                            FfiDeclarationRejection::UnknownProperty | FfiDeclarationRejection::InvalidValue
                        ) {
                            errors.push(index);
                        }
                    }
                }
                ParsedRuleChild::Rule(rule) => {
                    if rule.is_qualified && rule.selector_list.is_none() {
                        return;
                    }
                    for child in &rule.children {
                        visit_item(sheet, child, errors);
                    }
                }
            }
        }
        fn visit_rule(sheet: &ParsedStyleSheet, rule: &ParsedRule, errors: &mut Vec<usize>) {
            match rule.rule_kind {
                ParsedRuleKind::Qualified if rule.selector_list.is_some() => {}
                ParsedRuleKind::Container
                | ParsedRuleKind::Media
                | ParsedRuleKind::Supports
                | ParsedRuleKind::Scope
                | ParsedRuleKind::Layer
                | ParsedRuleKind::Function
                | ParsedRuleKind::Page => {}
                _ => return,
            }
            for child in &rule.children {
                match child {
                    ParsedRuleChild::Declarations(_) => visit_item(sheet, child, errors),
                    ParsedRuleChild::Rule(rule) => visit_rule(sheet, rule, errors),
                }
            }
        }
        let mut errors = Vec::new();
        for rule in &self.rules {
            visit_rule(self, rule, &mut errors);
        }
        for item in &self.items {
            visit_item(self, item, &mut errors);
        }
        errors.into_boxed_slice()
    }

    fn native_declarations(
        &self,
        declarations: &ParsedDeclarationList,
        declarations_kind: NestedDeclarationsKind,
        list: &NativeRuleList,
    ) -> Rc<NativeRule> {
        let (kind, payload, position) = match declarations_kind {
            NestedDeclarationsKind::Style => (
                NativeRuleType::NestedDeclarations,
                RulePayload::NestedDeclarations(Box::new(DeclarationBlock::new(
                    declarations.declaration_block.as_ref().unwrap().clone(),
                ))),
                (declarations.count != 0).then(|| {
                    let first = &self.declarations[declarations.start];
                    (first.start_line, first.start_column)
                }),
            ),
            NestedDeclarationsKind::Function => (
                NativeRuleType::FunctionDeclarations,
                RulePayload::FunctionDeclarations(Box::new(FfiDescriptorBlock::new(
                    declarations.descriptor_block.as_ref().unwrap().clone(),
                ))),
                None,
            ),
        };
        NativeRule::with_identity(
            list.identity_for(declarations.local_identity),
            kind,
            payload,
            None,
            position,
        )
    }

    fn native_rule(
        self: &Arc<Self>,
        rule: &Arc<ParsedRule>,
        declarations_kind: NestedDeclarationsKind,
        list: &NativeRuleList,
    ) -> Option<Rc<NativeRule>> {
        let kind = rule.native_type()?;
        let child_rules = kind
            .child_declarations_kind(declarations_kind)
            .map(|kind| list.parsed_children(ParsedRuleList::children(self.clone(), rule.clone(), kind)));
        let position =
            (kind == NativeRuleType::Style && rule.has_source_position).then_some((rule.start_line, rule.start_column));
        let payload = match kind {
            NativeRuleType::Style => RulePayload::Style(StyleRule::new(
                rule.selector_list.as_ref().unwrap().clone(),
                rule.declaration_block.as_ref().unwrap().clone(),
            )),
            NativeRuleType::Keyframe => {
                RulePayload::Keyframe(Box::new(FfiKeyframe::new(rule.keyframe.as_ref().unwrap().clone())))
            }
            NativeRuleType::Import => RulePayload::Import {
                data: rule.import_rule.as_ref().unwrap().clone(),
                media: list
                    .import_media_for(list.identity_for(rule.local_identity))
                    .unwrap_or_else(|| MediaList::new(rule.media_list.as_ref().unwrap().clone())),
                scope: rule.scope_selectors.clone(),
            },
            NativeRuleType::Media => RulePayload::Media {
                list: MediaList::new(rule.media_list.as_ref().unwrap().clone()),
            },
            NativeRuleType::Supports => RulePayload::Supports(rule.supports_condition.as_ref().unwrap().clone()),
            NativeRuleType::Container => RulePayload::Container(rule.container_conditions.as_ref().unwrap().clone()),
            NativeRuleType::Scope => RulePayload::Scope(rule.scope_selectors.as_ref().unwrap().clone()),
            NativeRuleType::LayerBlock | NativeRuleType::LayerStatement => {
                RulePayload::LayerNames(rule.layer_names.as_ref().unwrap().clone())
            }
            NativeRuleType::Function => RulePayload::Function(rule.function_signature.as_ref().unwrap().clone()),
            NativeRuleType::Page => RulePayload::Page {
                selectors: RefCell::new(rule.page_selector_list.as_ref().unwrap().clone()),
                descriptors: Box::new(FfiDescriptorBlock::new(rule.descriptor_block.as_ref().unwrap().clone())),
            },
            NativeRuleType::Margin => {
                let name: Vec<u16> = self.values[rule.name_offset..rule.name_offset + rule.name_length]
                    .iter()
                    .map(|&unit| {
                        if (u16::from(b'A')..=u16::from(b'Z')).contains(&unit) {
                            unit + u16::from(b'a' - b'A')
                        } else {
                            unit
                        }
                    })
                    .collect();
                RulePayload::Margin {
                    name: CssString::from_utf16(&name),
                    declarations: Box::new(DeclarationBlock::new(rule.declaration_block.as_ref().unwrap().clone())),
                }
            }
            NativeRuleType::FontFace => RulePayload::FontFace(Box::new(FfiDescriptorBlock::new(
                rule.descriptor_block.as_ref().unwrap().clone(),
            ))),
            NativeRuleType::FontFeatureValues => RulePayload::FontFeatureValues(Box::new(FontFeatureValuesRule::new(
                rule.font_feature_values.as_ref().unwrap().clone(),
            ))),
            NativeRuleType::Keyframes => {
                RulePayload::Keyframes(RefCell::new(rule.keyframes_name.as_ref().unwrap().clone()))
            }
            NativeRuleType::Namespace => RulePayload::Namespace(rule.namespace_rule.as_ref().unwrap().clone()),
            NativeRuleType::CounterStyle => RulePayload::CounterStyle(Box::new(FfiCounterStyle::new(
                rule.counter_style.as_ref().unwrap().clone(),
            ))),
            NativeRuleType::Property => RulePayload::Property(rule.property_rule.as_ref().unwrap().clone()),
            NativeRuleType::NestedDeclarations | NativeRuleType::FunctionDeclarations => unreachable!(),
        };
        Some(NativeRule::with_identity(
            list.identity_for(rule.local_identity),
            kind,
            payload,
            child_rules,
            position,
        ))
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `parse` must point to a live stylesheet owned by an `Arc`.
pub unsafe extern "C" fn rust_css_syntax_native_rules(parse: *const ParsedStyleSheet) -> *const NativeRuleList {
    unsafe { Arc::increment_strong_count(parse) };
    Rc::into_raw(NativeRuleList::from_parsed(unsafe { Arc::from_raw(parse) }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_syntax_root_count(parse: &ParsedStyleSheet) -> usize {
    parse.rules.len() + parse.items.len()
}

// These callbacks borrow the immutable parser result, without creating compatibility views.
#[unsafe(no_mangle)]
pub extern "C" fn rust_css_syntax_visit_diagnostics(
    parse: &ParsedStyleSheet,
    callback: extern "C" fn(*const u16, usize, &FfiSyntaxDiagnostic),
) {
    for diagnostic in &parse.diagnostics {
        callback(parse.values.as_ptr(), parse.values.len(), diagnostic);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_syntax_visit_declaration_errors(
    parse: &ParsedStyleSheet,
    callback: extern "C" fn(*const u16, usize, &FfiSyntaxDeclaration),
) {
    for &index in &parse.declaration_errors {
        callback(
            parse.values.as_ptr(),
            parse.values.len(),
            &parse.declarations[index].ffi_view(),
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_syntax_visit_root_declarations(
    parse: &ParsedStyleSheet,
    context: *mut c_void,
    callback: extern "C" fn(*mut c_void, *const u16, usize, &FfiSyntaxDeclaration),
) {
    for item in &parse.items {
        let ParsedRuleChild::Declarations(list) = item else {
            continue;
        };
        for declaration in &parse.declarations[list.start..list.start + list.count] {
            callback(
                context,
                parse.values.as_ptr(),
                parse.values.len(),
                &declaration.ffi_view(),
            );
        }
    }
}

impl ParsedDeclaration {
    fn ffi_view(&self) -> FfiSyntaxDeclaration {
        FfiSyntaxDeclaration {
            name_offset: self.name_offset,
            name_length: self.name_length,
            value_source_offset: self.value_source_offset,
            value_source_length: self.value_source_length,
            is_property: self.is_property,
            important: self.important,
            start_line: self.start_line,
            start_column: self.start_column,
            preserve_source_text: self.preserve_source_text,
            property_id: self.property_id,
            descriptor_id: self.descriptor_id,
            rejection: self.rejection,
            parsed_value: self.parsed_value.as_ref().map_or(std::ptr::null(), Arc::as_ptr).cast(),
        }
    }
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

fn append_collected_descriptor(descriptors: &mut Vec<DescriptorData>, descriptor: DescriptorData) {
    if let Some(index) = descriptors.iter().position(|existing| {
        existing.id == descriptor.id && (descriptor.id != CUSTOM_DESCRIPTOR_ID || existing.name == descriptor.name)
    }) {
        descriptors.remove(index);
    }
    descriptors.push(descriptor);
}

fn collect_descriptors<'a>(
    declarations: impl IntoIterator<Item = &'a Declaration>,
    context: &ParseContext,
) -> Vec<DescriptorData> {
    let mut descriptors = Vec::new();
    for declaration in declarations {
        let Some(at_rule) = descriptor_at_rule(declaration.rule_context) else {
            continue;
        };
        let source = component_list_source(&declaration.value);
        let Some(descriptor) = parse_descriptor(
            context,
            at_rule,
            declaration.name.as_ref(),
            &declaration.value,
            source.as_ref(),
        ) else {
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
                    DescriptorData {
                        id: longhand.descriptor_id,
                        name: CssString::from_utf16(declaration.name.as_ref()),
                        value: values.as_slice()[index].clone().into_arc(),
                    },
                );
            }
            continue;
        }
        append_collected_descriptor(
            &mut descriptors,
            DescriptorData {
                id: descriptor.id,
                name: CssString::from_utf16(declaration.name.as_ref()),
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

// NB: Mirrors the rule structural validation and top-level ordering that this series removed from
//     Libraries/LibWeb/CSS/Parser/RuleParsing.cpp and Libraries/LibWeb/CSS/Parser/Parser.cpp.
fn structural_validation_code(
    rule: &Rule,
    rule_kind: ParsedRuleKind,
    parsed_prelude: &ParsedRulePrelude,
    selector_list_is_valid: bool,
) -> Option<FfiSyntaxDiagnosticCode> {
    let valid_in_context = match rule {
        Rule::At(rule) => rule.valid_in_context,
        Rule::Qualified(rule) => rule.valid_in_context,
    };
    if !valid_in_context {
        return Some(FfiSyntaxDiagnosticCode::InvalidRuleContext);
    }

    match (rule, rule_kind) {
        (Rule::Qualified(rule), ParsedRuleKind::Qualified) if rule.prelude_is_selector => {
            if non_whitespace(&rule.prelude).is_empty() {
                Some(FfiSyntaxDiagnosticCode::StyleEmptySelector)
            } else if !selector_list_is_valid {
                Some(FfiSyntaxDiagnosticCode::StyleSelectorsInvalid)
            } else {
                None
            }
        }
        (Rule::Qualified(_), ParsedRuleKind::Qualified) => {
            (!matches!(parsed_prelude, ParsedRulePrelude::KeyframeSelectors(_)))
                .then_some(FfiSyntaxDiagnosticCode::KeyframeSelectorsInvalid)
        }
        (Rule::At(_), ParsedRuleKind::Unknown | ParsedRuleKind::IgnoredVendor) => None,
        (Rule::At(rule), ParsedRuleKind::Import) => (rule.has_block
            || !matches!(parsed_prelude, ParsedRulePrelude::Import(_)))
        .then_some(FfiSyntaxDiagnosticCode::ImportInvalid),
        (Rule::At(rule), ParsedRuleKind::Layer) => {
            if rule
                .outer_rule_name
                .as_deref()
                .is_some_and(|name| equals_ascii_case_insensitive(name, b"style"))
                && !rule.has_block
            {
                Some(FfiSyntaxDiagnosticCode::InvalidRuleContext)
            } else if rule.has_block && !matches!(parsed_prelude, ParsedRulePrelude::Name(_)) {
                Some(FfiSyntaxDiagnosticCode::LayerInvalidName)
            } else if !rule.has_block && !matches!(parsed_prelude, ParsedRulePrelude::Names(_)) {
                Some(FfiSyntaxDiagnosticCode::LayerContainsInvalidName)
            } else {
                None
            }
        }
        (Rule::At(rule), ParsedRuleKind::Keyframes) => {
            if !rule.has_block {
                Some(FfiSyntaxDiagnosticCode::KeyframesMustBeBlock)
            } else if !matches!(parsed_prelude, ParsedRulePrelude::Name(_)) {
                Some(FfiSyntaxDiagnosticCode::KeyframesInvalidName)
            } else {
                None
            }
        }
        (Rule::At(rule), ParsedRuleKind::Namespace) => {
            if rule.has_block {
                Some(FfiSyntaxDiagnosticCode::NamespaceMustBeStatement)
            } else if !matches!(parsed_prelude, ParsedRulePrelude::Namespace { .. }) {
                Some(FfiSyntaxDiagnosticCode::NamespaceInvalidPrelude)
            } else {
                None
            }
        }
        (Rule::At(rule), ParsedRuleKind::Media) => {
            (!rule.has_block).then_some(FfiSyntaxDiagnosticCode::MediaExpectedBlock)
        }
        (Rule::At(rule), ParsedRuleKind::Supports) => {
            if !rule.has_block {
                Some(FfiSyntaxDiagnosticCode::SupportsMustBeBlock)
            } else if !matches!(parsed_prelude, ParsedRulePrelude::SupportsCondition(_)) {
                Some(FfiSyntaxDiagnosticCode::SupportsClauseInvalid)
            } else {
                None
            }
        }
        (Rule::At(rule), ParsedRuleKind::Container) => {
            if !rule.has_block {
                Some(FfiSyntaxDiagnosticCode::ContainerMustBeBlock)
            } else if !matches!(parsed_prelude, ParsedRulePrelude::ContainerConditions(_)) {
                Some(FfiSyntaxDiagnosticCode::ContainerConditionsInvalid)
            } else {
                None
            }
        }
        (Rule::At(rule), ParsedRuleKind::CounterStyle) => {
            if !rule.has_block {
                Some(FfiSyntaxDiagnosticCode::CounterStyleMustBeBlock)
            } else if !matches!(parsed_prelude, ParsedRulePrelude::Name(_)) {
                Some(FfiSyntaxDiagnosticCode::CounterStyleMissingName)
            } else {
                None
            }
        }
        (Rule::At(rule), ParsedRuleKind::FontFace) => {
            if !rule.has_block {
                Some(FfiSyntaxDiagnosticCode::FontFaceMustBeBlock)
            } else if !matches!(parsed_prelude, ParsedRulePrelude::Empty) {
                Some(FfiSyntaxDiagnosticCode::FontFacePreludeNotAllowed)
            } else {
                None
            }
        }
        (Rule::At(rule), ParsedRuleKind::FontFeatureValues) => {
            if !rule.has_block {
                Some(FfiSyntaxDiagnosticCode::FontFeatureValuesMustBeBlock)
            } else if !matches!(parsed_prelude, ParsedRulePrelude::FontFamilyNames(_)) {
                Some(FfiSyntaxDiagnosticCode::FontFeatureValuesPreludeInvalid)
            } else {
                None
            }
        }
        (Rule::At(rule), ParsedRuleKind::Function) => {
            if !rule.has_block {
                Some(FfiSyntaxDiagnosticCode::FunctionMustBeBlock)
            } else if !matches!(parsed_prelude, ParsedRulePrelude::Function { .. }) {
                Some(FfiSyntaxDiagnosticCode::FunctionPreludeInvalid)
            } else {
                None
            }
        }
        (Rule::At(rule), ParsedRuleKind::Page) => {
            if !rule.has_block {
                Some(FfiSyntaxDiagnosticCode::PageMustBeBlock)
            } else if !matches!(parsed_prelude, ParsedRulePrelude::PageSelectors(_)) {
                Some(FfiSyntaxDiagnosticCode::PagePreludeInvalid)
            } else {
                None
            }
        }
        (Rule::At(rule), ParsedRuleKind::Margin) => {
            if !rule.has_block {
                Some(FfiSyntaxDiagnosticCode::MarginMustBeBlock)
            } else if !matches!(parsed_prelude, ParsedRulePrelude::Empty) {
                Some(FfiSyntaxDiagnosticCode::MarginPreludeNotAllowed)
            } else {
                None
            }
        }
        (Rule::At(rule), ParsedRuleKind::Property) => (!rule.has_block
            || !matches!(parsed_prelude, ParsedRulePrelude::Property { .. }))
        .then_some(FfiSyntaxDiagnosticCode::PropertyPreludeInvalid),
        (Rule::At(rule), ParsedRuleKind::Scope) => (!rule.has_block
            || !matches!(parsed_prelude, ParsedRulePrelude::Scope { .. }))
        .then_some(FfiSyntaxDiagnosticCode::ScopeInvalid),
        (Rule::At(_), ParsedRuleKind::FontFeatureValuesRule) => None,
        // NB: `at_rule_kind` never classifies a rule as Qualified or Invalid, and qualified rules
        //     are handled above, so every remaining pair is a classifier bug rather than input.
        (rule, kind) => unreachable!(
            "unclassified rule: {} rule with kind {kind:?}",
            match rule {
                Rule::At(_) => "at",
                Rule::Qualified(_) => "qualified",
            }
        ),
    }
}

fn rule_outer_name(rule: &Rule) -> Option<&ParserString> {
    match rule {
        Rule::At(rule) => rule.outer_rule_name.as_ref(),
        Rule::Qualified(rule) => rule.outer_rule_name.as_ref(),
    }
}

fn rule_diagnostic_name(rule: &Rule) -> ParserString {
    match rule {
        Rule::At(rule) => at_rule_name(rule.name.as_ref()),
        Rule::Qualified(rule) if rule.prelude_is_selector => utf16_from_ascii(b"style"),
        Rule::Qualified(_) => utf16_from_ascii(b"keyframe"),
    }
}

fn invalid_location_inner_name(rule: &Rule, rule_kind: ParsedRuleKind, outer_name: &[u16]) -> ParserString {
    if equals_ascii_case_insensitive(outer_name, b"keyframe") {
        return utf16_from_ascii(b"qualified-rule");
    }
    if matches!(rule, Rule::At(_)) && rule_kind == ParsedRuleKind::Layer {
        return utf16_from_ascii(b"CSSLayerStatementRule");
    }
    match rule {
        Rule::At(rule) => at_rule_name(rule.name.as_ref()),
        Rule::Qualified(_) => utf16_from_ascii(b"qualified-rule"),
    }
}

impl SyntaxParseBuilder {
    fn finish(self) -> ParsedStyleSheet {
        let mut parsed = ParsedStyleSheet {
            identity_count: self.identity_count,
            cache_metadata: None,
            declarations_kind: self.declarations_kind,
            values: self.values.into_boxed_slice(),
            declarations: self.declarations.into_boxed_slice(),
            rules: self.rules.into_boxed_slice(),
            items: self.items.into_boxed_slice(),
            native_roots: Box::default(),
            diagnostics: self.diagnostics.into_boxed_slice(),
            declaration_errors: Box::default(),
        };
        parsed.native_roots = if parsed.items.is_empty() {
            parsed
                .rules
                .iter()
                .enumerate()
                .filter_map(|(index, rule)| rule.native_type().map(|_| index))
                .collect()
        } else {
            shared_rules::native_child_indices(&parsed.items, None)
        };
        parsed.declaration_errors = parsed.collect_declaration_errors();
        parsed
    }

    pub(crate) fn new(parse_context: *const ParseContext, preserve_property_source_text: bool) -> Self {
        let declared_namespaces = (unsafe { parse_context.as_ref() })
            .map(|context| unsafe { declared_namespaces_from_context(context) })
            .unwrap_or_default()
            .into_iter()
            .map(|namespace| {
                let mut code_units = Vec::with_capacity(namespace.len());
                namespace.append_to(&mut code_units);
                ParserString::from(code_units.into_boxed_slice())
            })
            .collect();
        Self {
            values: Vec::new(),
            declarations_kind: NestedDeclarationsKind::Style,
            declarations: Vec::new(),
            rules: Vec::new(),
            items: Vec::new(),
            identity_count: 0,
            font_feature_values: Vec::new(),
            diagnostics: Vec::new(),
            declared_namespaces,
            parse_context,
            preserve_property_source_text,
        }
    }

    pub(crate) fn append_value(&mut self, value: &[u16]) -> (usize, usize) {
        let offset = self.values.len();
        self.values.extend_from_slice(value);
        (offset, value.len())
    }

    fn append_diagnostic(
        &mut self,
        code: FfiSyntaxDiagnosticCode,
        primary: Option<&[u16]>,
        secondary: Option<&[u16]>,
        prelude: Option<&[u16]>,
    ) {
        let (primary_offset, primary_length) = self.append_optional_value(primary);
        let (secondary_offset, secondary_length) = self.append_optional_value(secondary);
        let (prelude_offset, prelude_length) = self.append_optional_value(prelude);
        self.diagnostics.push(FfiSyntaxDiagnostic {
            code,
            start_line: 0,
            start_column: 0,
            end_line: 0,
            end_column: 0,
            primary_offset,
            primary_length,
            secondary_offset,
            secondary_length,
            prelude_offset,
            prelude_length,
        });
    }

    fn append_rule_validation_diagnostic(
        &mut self,
        rule: &Rule,
        rule_kind: ParsedRuleKind,
        code: FfiSyntaxDiagnosticCode,
        prelude: &[u16],
    ) {
        if code == FfiSyntaxDiagnosticCode::InvalidRuleContext
            && let Some(outer_name) = rule_outer_name(rule)
        {
            let inner_name = invalid_location_inner_name(rule, rule_kind, outer_name.as_ref());
            self.append_diagnostic(
                FfiSyntaxDiagnosticCode::InvalidRuleLocation,
                Some(outer_name.as_ref()),
                Some(inner_name.as_ref()),
                None,
            );
            return;
        }
        let rule_name = rule_diagnostic_name(rule);
        self.append_diagnostic(code, Some(rule_name.as_ref()), None, Some(prelude));
    }

    fn append_optional_value(&mut self, value: Option<&[u16]>) -> (usize, usize) {
        value.map_or((usize::MAX, 0), |value| self.append_value(value))
    }

    fn append_declaration(
        &mut self,
        declaration: &Declaration,
        font_feature_maximum_value_count: Option<usize>,
    ) -> usize {
        let (name_offset, name_length) = self.append_value(declaration.name.as_ref());
        let value_source = component_list_source(&declaration.value);
        let (property_id, descriptor_id, rejection, parsed_value) =
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
        let needs_value_text = self.preserve_property_source_text || rejection == FfiDeclarationRejection::InvalidValue;
        let (value_source_offset, value_source_length) = if needs_value_text {
            self.append_value(value_source.as_ref())
        } else {
            (0, 0)
        };
        let index = self.declarations.len();
        self.declarations.push(ParsedDeclaration {
            name_offset,
            name_length,
            value_source_offset,
            value_source_length,
            is_property: declaration.is_property,
            important: declaration.important,
            start_line: declaration.source_position.line,
            start_column: declaration.source_position.column,
            preserve_source_text: self.preserve_property_source_text,
            property_id,
            descriptor_id,
            rejection,
            parsed_value,
            font_feature_values_start,
            font_feature_value_count,
        });
        index
    }

    fn parse_declaration_value(
        &mut self,
        declaration: &Declaration,
        source_utf16: &[u16],
    ) -> (u16, u8, FfiDeclarationRejection, Option<Arc<StyleValueData>>) {
        if self.parse_context.is_null() {
            return (u16::MAX, u8::MAX, FfiDeclarationRejection::None, None);
        }
        if !declaration.is_property {
            let Some(at_rule) = descriptor_at_rule(declaration.rule_context) else {
                return (u16::MAX, u8::MAX, FfiDeclarationRejection::None, None);
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
                    FfiDeclarationRejection::None,
                    Some(descriptor.value),
                ),
                None => (u16::MAX, u8::MAX, FfiDeclarationRejection::None, None),
            };
        }
        let Some(property_id) = property_id_from_name(declaration.name.as_ref()) else {
            // NB: Mirrors the ignored-vendor and unknown-property distinction from commit
            //     dbe9950abd9 in Libraries/LibWeb/CSS/Parser/Parser.cpp:360.
            let rejection = if has_ignored_vendor_prefix(declaration.name.as_ref().into()) {
                FfiDeclarationRejection::IgnoredVendorPrefix
            } else {
                FfiDeclarationRejection::UnknownProperty
            };
            return (u16::MAX, u8::MAX, rejection, None);
        };

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
            ParseOutcome::Parsed(value) => (property_id, u8::MAX, FfiDeclarationRejection::None, Some(value)),
            ParseOutcome::Invalid | ParseOutcome::NotHandled => {
                (property_id, u8::MAX, FfiDeclarationRejection::InvalidValue, None)
            }
        }
    }

    fn append_collected_declaration(&mut self, descriptor: &DescriptorData) {
        let (name_offset, name_length) = self.append_value(descriptor.name.units());
        self.declarations.push(ParsedDeclaration {
            name_offset,
            name_length,
            value_source_offset: 0,
            value_source_length: 0,
            is_property: false,
            important: false,
            start_line: 0,
            start_column: 0,
            preserve_source_text: false,
            property_id: u16::MAX,
            descriptor_id: descriptor.id,
            rejection: FfiDeclarationRejection::None,
            parsed_value: Some(descriptor.value.clone()),
            font_feature_values_start: usize::MAX,
            font_feature_value_count: 0,
        });
    }

    fn append_declarations(
        &mut self,
        declarations: &[Declaration],
        font_feature_maximum_value_count: Option<usize>,
    ) -> (usize, usize, Option<Arc<DescriptorBlockData>>) {
        let start = self.declarations.len();
        if !self.parse_context.is_null()
            && declarations
                .first()
                .is_some_and(|declaration| descriptor_at_rule(declaration.rule_context).is_some())
        {
            let context = unsafe { &*self.parse_context };
            let descriptors = collect_descriptors(declarations, context);
            for descriptor in &descriptors {
                self.append_collected_declaration(descriptor);
            }
            return (
                start,
                self.declarations.len() - start,
                Some(Arc::new(DescriptorBlockData { descriptors })),
            );
        }
        for declaration in declarations {
            self.append_declaration(declaration, font_feature_maximum_value_count);
        }
        (start, declarations.len(), None)
    }

    fn append_items(
        &mut self,
        items: &[RuleOrDeclarations],
        font_feature_maximum_value_count: Option<usize>,
    ) -> Box<[ParsedRuleChild]> {
        items
            .iter()
            .map(|item| self.append_item(item, font_feature_maximum_value_count))
            .collect()
    }

    fn append_declaration_block(
        &self,
        block: &mut DeclarationBlockData,
        start: usize,
        count: usize,
        ignore_important: bool,
    ) {
        for declaration in &self.declarations[start..start + count] {
            if !declaration.is_property {
                continue;
            }
            // https://drafts.csswg.org/css-animations-1/#keyframes
            // None of the properties [in the <keyframe-block>'s <declaration-list>] interact with the cascade (so
            // using !important on them is invalid and will cause the property to be ignored).
            if ignore_important && declaration.important {
                continue;
            }
            let Some(value) = &declaration.parsed_value else {
                continue;
            };
            let property = DeclaredProperty {
                property_id: declaration.property_id,
                important: declaration.important,
                value: value.clone(),
            };
            if declaration.property_id == property_id::CUSTOM {
                let name = &self.values[declaration.name_offset..declaration.name_offset + declaration.name_length];
                block.set_custom_property(CssString::from_utf16(name), property);
            } else {
                block.append_in_specified_order(property);
            }
        }
    }

    fn next_identity(&mut self) -> u64 {
        let identity = self.identity_count;
        self.identity_count = identity.checked_add(1).expect("parsed rule identity overflow");
        identity
    }

    fn append_item(
        &mut self,
        item: &RuleOrDeclarations,
        font_feature_maximum_value_count: Option<usize>,
    ) -> ParsedRuleChild {
        let declarations = match item {
            RuleOrDeclarations::Rule(rule) => return ParsedRuleChild::Rule(self.append_rule(rule)),
            RuleOrDeclarations::Declarations(declarations) => declarations,
        };
        let (start, count, descriptor_block) = self.append_declarations(declarations, font_feature_maximum_value_count);
        let mut block = DeclarationBlockData::default();
        self.append_declaration_block(
            &mut block,
            start,
            count,
            declarations
                .first()
                .is_some_and(|declaration| declaration.rule_context == RuleContext::Keyframe),
        );
        let declaration_block = Some(Arc::new(block));
        ParsedRuleChild::Declarations(ParsedDeclarationList {
            local_identity: self.next_identity(),
            start,
            count,
            declaration_block,
            descriptor_block,
        })
    }

    fn parse_selector_list(
        &self,
        values: &[ComponentValue],
        selector_type: SelectorType,
        nesting_parent: StyleNestingParent,
    ) -> Option<Arc<RustParsedSelectorList>> {
        let declared_namespaces = self
            .declared_namespaces
            .iter()
            .map(|namespace| TokenizerInput::Utf16(namespace.as_ref()))
            .collect::<Vec<_>>();
        let selector_list =
            parse_selector_list_from_component_values(values, &declared_namespaces, selector_type).ok()?;
        Some(Arc::new(selector_list.adapt_for_nesting(nesting_parent)))
    }

    fn rule_descriptors(children: &[ParsedRuleChild], kind: ParsedRuleKind) -> Option<Arc<DescriptorBlockData>> {
        let mut data: Option<Arc<DescriptorBlockData>> = None;
        for child in children {
            let ParsedRuleChild::Declarations(child) = child else {
                continue;
            };
            let Some(block) = &child.descriptor_block else { continue };
            if let Some(data) = &mut data {
                for descriptor in &block.descriptors {
                    append_collected_descriptor(&mut Arc::make_mut(data).descriptors, descriptor.clone());
                }
            } else {
                data = Some(block.clone());
            }
        }
        if data.as_ref().is_none_or(|data| data.descriptors.is_empty())
            && !matches!(
                kind,
                ParsedRuleKind::FontFace | ParsedRuleKind::Page | ParsedRuleKind::CounterStyle
            )
        {
            return None;
        }
        Some(data.unwrap_or_default())
    }

    fn append_rule(&mut self, rule: &Rule) -> Arc<ParsedRule> {
        let original_rule_kind = rule_kind(rule);
        let (is_qualified, name, prelude, declarations, children, has_block, source_position) = match rule {
            Rule::At(rule) => (
                false,
                rule.name.as_ref(),
                rule.prelude.as_slice(),
                &[][..],
                rule.children.as_slice(),
                rule.has_block,
                None,
            ),
            Rule::Qualified(rule) => (
                true,
                &[][..],
                rule.prelude.as_slice(),
                rule.declarations.as_slice(),
                rule.children.as_slice(),
                true,
                rule.source_position,
            ),
        };
        let (name_offset, name_length) = self.append_value(name);
        let prelude_source = component_list_source(prelude);
        let font_feature_maximum_value_count = match rule {
            Rule::At(rule) => font_feature_values_rule(rule.name.as_ref()).map(|(_, maximum)| maximum),
            Rule::Qualified(_) => None,
        };
        let (declarations_start, declaration_count, _) = self.append_declarations(declarations, None);
        let children = self.append_items(children, font_feature_maximum_value_count);
        let descriptor_block = Self::rule_descriptors(&children, original_rule_kind);
        let selector_list = match rule {
            Rule::Qualified(rule) if rule.prelude_is_selector => self.parse_selector_list(
                &rule.prelude,
                if rule.nesting_parent != StyleNestingParent::None {
                    SelectorType::Relative
                } else {
                    SelectorType::Standalone
                },
                rule.nesting_parent,
            ),
            _ => None,
        };
        let parse_context = unsafe { self.parse_context.as_ref() };
        let declared_namespaces = self
            .declared_namespaces
            .iter()
            .map(|namespace| TokenizerInput::Utf16(namespace.as_ref()))
            .collect::<Vec<_>>();
        let parsed_prelude = parse_rule_prelude(
            rule,
            original_rule_kind,
            parse_context,
            &declared_namespaces,
            &resolve_query_feature,
        );
        let validation_code =
            structural_validation_code(rule, original_rule_kind, &parsed_prelude, selector_list.is_some());
        let rule_kind = if let Some(code) = validation_code {
            self.append_rule_validation_diagnostic(rule, original_rule_kind, code, prelude_source.as_ref());
            ParsedRuleKind::Invalid
        } else {
            if original_rule_kind == ParsedRuleKind::Unknown {
                let rule_name = rule_diagnostic_name(rule);
                self.append_diagnostic(
                    FfiSyntaxDiagnosticCode::UnknownRule,
                    Some(rule_name.as_ref()),
                    None,
                    None,
                );
            }
            original_rule_kind
        };
        let mut parsed_prelude_name = None;
        let mut page_selector_list = None;
        let mut import_rule = None;
        let mut supports_condition = None;
        let mut scope_selectors = None;
        let mut media_list = None;
        let mut layer_names = None;
        let mut container_conditions = None;
        let mut font_feature_values = None;
        let mut keyframe_keys = None;
        let mut namespace_rule = None;
        let mut property_rule = None;
        let mut function_signature = None;
        match parsed_prelude {
            ParsedRulePrelude::Unparsed | ParsedRulePrelude::Invalid | ParsedRulePrelude::Empty => {}
            ParsedRulePrelude::Name(name) => {
                if rule_kind == ParsedRuleKind::Layer {
                    layer_names = Some(Arc::new(LayerNames {
                        names: vec![CssString::from_utf16(&name)].into_boxed_slice(),
                    }));
                } else {
                    parsed_prelude_name = Some(name);
                }
            }
            ParsedRulePrelude::Names(names) => {
                if rule_kind == ParsedRuleKind::Layer {
                    layer_names = Some(Arc::new(LayerNames {
                        names: names.iter().map(|name| CssString::from_utf16(name)).collect(),
                    }));
                }
            }
            ParsedRulePrelude::KeyframeSelectors(selectors) => {
                keyframe_keys = Some(selectors.into_boxed_slice());
            }
            ParsedRulePrelude::Namespace { prefix, uri } => {
                namespace_rule = Some(Arc::new(NamespaceRuleData {
                    prefix: CssString::from_utf16(prefix.as_deref().unwrap_or_default()),
                    uri: CssString::from_utf16(&uri),
                }));
            }
            ParsedRulePrelude::PageSelectors(selectors) => {
                page_selector_list = Some(Arc::new(PageSelectorList::new(selectors)));
            }
            ParsedRulePrelude::FontFamilyNames(names) => {
                let mut data = FontFeatureValuesData {
                    families: names.iter().map(|name| CssString::from_utf16(name)).collect(),
                    ..Default::default()
                };
                for child in &children {
                    // FIXME: Handle the font-display descriptor here, see
                    //        https://drafts.csswg.org/css-fonts-4/#font-display-font-feature-values
                    let ParsedRuleChild::Rule(child) = child else {
                        continue;
                    };
                    if child.rule_kind != ParsedRuleKind::FontFeatureValuesRule {
                        continue;
                    }
                    let name = &self.values[child.name_offset..child.name_offset + child.name_length];
                    let (kind, _) = font_feature_values_rule(name).unwrap();
                    for item in &child.children {
                        let ParsedRuleChild::Declarations(item) = item else {
                            continue;
                        };
                        for declaration in &self.declarations[item.start..item.start + item.count] {
                            if declaration.font_feature_value_count == 0 {
                                continue;
                            }
                            let name = &self.values
                                [declaration.name_offset..declaration.name_offset + declaration.name_length];
                            let values = &self.font_feature_values[declaration.font_feature_values_start
                                ..declaration.font_feature_values_start + declaration.font_feature_value_count];
                            data.set(kind, name, values.to_vec());
                        }
                    }
                }
                font_feature_values = Some(Arc::new(data));
            }
            ParsedRulePrelude::Scope { start, end } => {
                scope_selectors = Some(Arc::new(ScopeSelectors {
                    start: start.map(Arc::new),
                    end: end.map(Arc::new),
                }));
            }
            ParsedRulePrelude::Import(import) => {
                if import.has_scope {
                    scope_selectors = Some(Arc::new(ScopeSelectors {
                        start: import.scope_start.map(Arc::new),
                        end: import.scope_end.map(Arc::new),
                    }));
                }
                import_rule = Some(Arc::new(ImportRuleData {
                    url: import.url.0,
                    layer: import.layer.map(|layer| CssString::from_utf16(&layer)),
                    supports: import
                        .supports
                        .map(|supports| expression_query_handle(supports, QueryKind::Supports)),
                    scope: scope_selectors.clone(),
                }));
                media_list = Some(Arc::new(MediaListData {
                    queries: import.media_queries.into_iter().map(media_query_handle).collect(),
                }));
            }
            ParsedRulePrelude::Function {
                name,
                parameters,
                return_type,
            } => {
                function_signature = Some(Arc::new(FunctionSignature {
                    name: CssString::from_utf16(&name),
                    parameters: parameters
                        .into_iter()
                        .map(|parameter| FunctionParameterData {
                            name: CssString::from_utf16(&parameter.name),
                            syntax: parameter.syntax,
                            default_value: parameter.default_value.map(|value| value.0),
                        })
                        .collect(),
                    return_type,
                }));
            }
            ParsedRulePrelude::Property {
                name,
                syntax_source,
                syntax,
                inherits,
                initial_value,
            } => {
                property_rule = Some(Arc::new(PropertyRuleData {
                    name: CssString::from_utf16(&name),
                    syntax_source: CssString::from_utf16(&syntax_source),
                    syntax,
                    inherits,
                    initial_value: initial_value.map(|value| value.0),
                }));
            }
            ParsedRulePrelude::MediaQueries(queries) => {
                media_list = Some(Arc::new(MediaListData {
                    queries: queries.into_iter().map(media_query_handle).collect(),
                }));
            }
            ParsedRulePrelude::SupportsCondition(expression) => {
                supports_condition = Some(expression_query_handle(expression, QueryKind::Supports));
            }
            ParsedRulePrelude::ContainerConditions(conditions) => {
                container_conditions = Some(Arc::new(ContainerConditionsData {
                    conditions: conditions
                        .into_iter()
                        .map(|condition| ContainerConditionData {
                            name: condition.name.map(|name| CssString::from_utf16(&name)),
                            query: condition
                                .query
                                .map(|expression| expression_query_handle(expression, QueryKind::Size)),
                        })
                        .collect(),
                }));
            }
            ParsedRulePrelude::FontFeatureValuesRule(_) => {}
        }
        let declaration_block = if let Rule::Qualified(qualified_rule) = rule {
            let mut block = DeclarationBlockData::default();
            self.append_declaration_block(
                &mut block,
                declarations_start,
                declaration_count,
                !qualified_rule.prelude_is_selector,
            );
            if !qualified_rule.prelude_is_selector {
                for item in &children {
                    if let ParsedRuleChild::Declarations(item) = item {
                        self.append_declaration_block(&mut block, item.start, item.count, true);
                    }
                }
            }
            Some(Arc::new(block))
        } else if rule_kind == ParsedRuleKind::Margin {
            Some(declaration_block_from_blocks(children.iter().filter_map(
                |item| match item {
                    ParsedRuleChild::Declarations(item) => item.declaration_block.as_ref(),
                    ParsedRuleChild::Rule(_) => None,
                },
            )))
        } else {
            None
        };
        let keyframe = if rule_kind == ParsedRuleKind::Qualified {
            keyframe_keys.map(|keys| {
                Arc::new(KeyframeData {
                    keys,
                    declarations: declaration_block.as_ref().unwrap().clone(),
                })
            })
        } else {
            None
        };
        let keyframes_name = (rule_kind == ParsedRuleKind::Keyframes)
            .then(|| CssString::from_utf16(parsed_prelude_name.as_deref().unwrap()));
        let counter_style = if rule_kind == ParsedRuleKind::CounterStyle {
            Some(Arc::new(CounterStyleData {
                name: CssString::from_utf16(parsed_prelude_name.as_deref().unwrap()),
                descriptors: descriptor_block.as_ref().unwrap().clone(),
            }))
        } else {
            None
        };
        let native_children = if keyframe.is_some() {
            Box::default()
        } else {
            shared_rules::native_child_indices(&children, Some(rule_kind))
        };
        Arc::new(ParsedRule {
            local_identity: self.next_identity(),
            is_qualified,
            rule_kind,
            name_offset,
            name_length,
            declaration_block,
            children,
            native_children,
            has_block,
            has_source_position: source_position.is_some(),
            start_line: source_position.map_or(0, |position| position.line),
            start_column: source_position.map_or(0, |position| position.column),
            page_selector_list,
            import_rule,
            supports_condition,
            selector_list,
            descriptor_block,
            layer_names,
            container_conditions,
            font_feature_values,
            keyframe,
            keyframes_name,
            scope_selectors,
            media_list,
            counter_style,
            namespace_rule,
            property_rule,
            function_signature,
        })
    }

    fn append_roots(&mut self, rules: &[Rule]) {
        // NB: Mirrors the top-level rule ordering that this series removed from
        //     Libraries/LibWeb/CSS/Parser/Parser.cpp.
        //     Invalid rules do not close either ordering window, and statement @layer rules do not
        //     close the import or namespace window.
        let mut import_rules_valid = true;
        let mut namespace_rules_valid = true;
        for rule in rules {
            let mut parsed = self.append_rule(rule);
            let rule_kind = parsed.rule_kind;
            match rule_kind {
                ParsedRuleKind::Invalid | ParsedRuleKind::Unknown | ParsedRuleKind::IgnoredVendor => {}
                ParsedRuleKind::Layer if !parsed.has_block => {}
                ParsedRuleKind::Import if import_rules_valid => {}
                ParsedRuleKind::Import => {
                    let prelude = component_list_source(match rule {
                        Rule::At(rule) => &rule.prelude,
                        Rule::Qualified(_) => unreachable!(),
                    });
                    self.append_rule_validation_diagnostic(
                        rule,
                        ParsedRuleKind::Import,
                        FfiSyntaxDiagnosticCode::MisplacedImport,
                        prelude.as_ref(),
                    );
                    Arc::get_mut(&mut parsed).unwrap().rule_kind = ParsedRuleKind::Invalid;
                }
                ParsedRuleKind::Namespace if namespace_rules_valid => {
                    import_rules_valid = false;
                    let prefix = parsed.namespace_rule.as_ref().unwrap().prefix.units();
                    if !self
                        .declared_namespaces
                        .iter()
                        .any(|namespace| namespace.as_ref() == prefix)
                    {
                        self.declared_namespaces.push(ParserString::from(Box::from(prefix)));
                    }
                }
                ParsedRuleKind::Namespace => {
                    import_rules_valid = false;
                    let prelude = component_list_source(match rule {
                        Rule::At(rule) => &rule.prelude,
                        Rule::Qualified(_) => unreachable!(),
                    });
                    self.append_rule_validation_diagnostic(
                        rule,
                        ParsedRuleKind::Namespace,
                        FfiSyntaxDiagnosticCode::MisplacedNamespace,
                        prelude.as_ref(),
                    );
                    Arc::get_mut(&mut parsed).unwrap().rule_kind = ParsedRuleKind::Invalid;
                }
                _ => {
                    import_rules_valid = false;
                    namespace_rules_valid = false;
                }
            }
            self.rules.push(parsed);
        }
    }

    fn append_root_items(&mut self, items: &[RuleOrDeclarations]) {
        self.items = items.iter().map(|item| self.append_item(item, None)).collect();
    }
}

fn token_diagnostics(tokens: &[ParserToken]) -> Vec<FfiSyntaxDiagnostic> {
    tokens
        .iter()
        .filter_map(|token| {
            let code = match &token.kind {
                ParserTokenKind::BadString => FfiSyntaxDiagnosticCode::BadString,
                ParserTokenKind::BadUrl => FfiSyntaxDiagnosticCode::BadUrl,
                _ => return None,
            };
            Some(FfiSyntaxDiagnostic {
                code,
                start_line: token.start_position.line,
                start_column: token.start_position.column,
                end_line: token.end_position.line,
                end_column: token.end_position.column,
                primary_offset: usize::MAX,
                primary_length: 0,
                secondary_offset: usize::MAX,
                secondary_length: 0,
                prelude_offset: usize::MAX,
                prelude_length: 0,
            })
        })
        .collect()
}

/// # Safety
/// The parse context and its referenced data must remain valid during parsing.
pub(crate) unsafe fn parse_shared_stylesheet(
    source: TokenizerInput<'_>,
    parse_context: *const ParseContext,
) -> Arc<ParsedStyleSheet> {
    unsafe {
        parse_with_cache(source, parse_context, || {
            let (mut parser, diagnostics) = Parser::from_source(source, Vec::new());
            let mut parse = SyntaxParseBuilder::new(parse_context, false);
            parse.diagnostics = diagnostics;
            let rules = parser.consume_stylesheet_contents();
            parse.append_roots(&rules);
            parse.finish()
        })
    }
}

fn rule_contexts(contexts: &[u8]) -> Option<Vec<RuleContext>> {
    contexts
        .iter()
        .map(|&context| {
            if context > RuleContext::Margin as u8 {
                return None;
            }
            // SAFETY: RuleContext has contiguous repr(u8) discriminants and the value was range-checked.
            Some(unsafe { std::mem::transmute::<u8, RuleContext>(context) })
        })
        .collect()
}

/// # Safety
/// The source and parse context must remain readable during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_css_stylesheet_syntax(
    source: FfiUtf16View,
    parse_context: *const ParseContext,
) -> *const ParsedStyleSheet {
    let Some(source) = (unsafe { source.units() }) else {
        return std::ptr::null();
    };
    Arc::into_raw(unsafe { parse_shared_stylesheet(source, parse_context) })
}

/// Parses exactly one CSS rule into a Rust-owned arena.
///
/// # Safety
/// The source, context array, and parse context must remain readable during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_css_rule_syntax(
    source: FfiUtf16View,
    contexts: *const u8,
    context_count: usize,
    nested: bool,
    parse_context: *const ParseContext,
) -> *const ParsedStyleSheet {
    let Some(source) = (unsafe { source.units() }) else {
        return std::ptr::null_mut();
    };
    let Some(contexts) = (unsafe { crate::bytes_from_raw(contexts, context_count) }) else {
        return std::ptr::null_mut();
    };
    let Some(contexts) = rule_contexts(contexts) else {
        return std::ptr::null_mut();
    };
    Arc::into_raw(unsafe { parse_shared_rule(source, contexts, nested, parse_context) })
}

/// # Safety
/// The parse context and its referenced data must remain valid during parsing.
pub(crate) unsafe fn parse_shared_rule(
    source: TokenizerInput<'_>,
    contexts: Vec<RuleContext>,
    nested: bool,
    parse_context: *const ParseContext,
) -> Arc<ParsedStyleSheet> {
    let (parser, diagnostics) = Parser::from_source(source, contexts);
    let mut parse = SyntaxParseBuilder::new(parse_context, false);
    if parser.rule_context.contains(&RuleContext::AtFunction) {
        parse.declarations_kind = NestedDeclarationsKind::Function;
    }
    parse.diagnostics = diagnostics;
    let rule = parse_rule_from_tokens(parser.tokens, parser.rule_context, nested);
    if let Some(rule) = rule {
        parse.append_roots(std::slice::from_ref(&rule));
    }
    Arc::new(parse.finish())
}

pub(crate) fn parse_keyframe_selector_list(source: TokenizerInput<'_>) -> Option<Vec<f64>> {
    let prelude = consume_a_list_of_component_values(tokenize_for_parser(source)).ok()?;
    match parse_keyframe_selectors(&prelude) {
        ParsedRulePrelude::KeyframeSelectors(selectors) => Some(selectors),
        _ => None,
    }
}

/// Parses an immutable CSS page selector list.
///
/// # Safety
/// `source` must remain readable for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_page_selector_list(source: FfiUtf16View) -> *const PageSelectorList {
    let Some(source) = (unsafe { source.units() }) else {
        return std::ptr::null();
    };
    let Some(selectors) = parse_page_selector_list(source) else {
        return std::ptr::null();
    };
    Arc::into_raw(Arc::new(PageSelectorList::new(selectors)))
}

/// # Safety
/// `list` must be null or point into a live `Arc<PageSelectorList>`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_page_selector_list_retain(list: *const PageSelectorList) -> *const PageSelectorList {
    if !list.is_null() {
        unsafe { Arc::increment_strong_count(list) };
    }
    list
}

/// # Safety
/// `list` must be null or own a reference to a live page-selector list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_page_selector_list_release(list: *const PageSelectorList) {
    if !list.is_null() {
        unsafe { Arc::decrement_strong_count(list) };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_page_selector_list_count(list: &PageSelectorList) -> usize {
    list.selectors.len()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_page_selector_list_serialize(
    list: &PageSelectorList,
    context: *mut c_void,
    visit: crate::css::parser::query_parser::VisitQuerySerialization,
) {
    let mut text = Vec::new();
    for (index, selector) in list.selectors.iter().enumerate() {
        if index != 0 {
            text.extend([u16::from(b','), u16::from(b' ')]);
        }
        if let Some(name) = &selector.name {
            text.extend_from_slice(name);
        }
        for pseudo_class in &selector.pseudo_classes {
            let name = match pseudo_class {
                PagePseudoClass::Left => ":left",
                PagePseudoClass::Right => ":right",
                PagePseudoClass::First => ":first",
                PagePseudoClass::Blank => ":blank",
            };
            text.extend(name.bytes().map(u16::from));
        }
    }
    unsafe { visit(context, text.as_ptr(), text.len()) };
}

/// Retains a normalized declaration block from parsed block contents, ignoring rules.
///
/// # Safety
/// `parse` must be a live result of `rust_parse_css_block_syntax`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_css_syntax_parse_declaration_block(
    parse: *const ParsedStyleSheet,
) -> *mut crate::css::declaration_block::DeclarationBlock {
    use crate::css::declaration_block::DeclarationBlock;
    let parsed = unsafe { &*parse };
    let declarations = declaration_block_from_blocks(parsed.items.iter().filter_map(|item| match item {
        ParsedRuleChild::Declarations(list) => list.declaration_block.as_ref(),
        ParsedRuleChild::Rule(_) => None,
    }));
    Box::into_raw(Box::new(DeclarationBlock::new(declarations)))
}

/// Retains parsed descriptor lists from block contents, ignoring rules.
///
/// # Safety
/// `parse` must be a live result of `rust_parse_css_block_syntax`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_css_syntax_parse_descriptor_block(
    parse: *const ParsedStyleSheet,
) -> *mut crate::css::descriptor_block::FfiDescriptorBlock {
    use crate::css::descriptor_block::FfiDescriptorBlock;
    let parsed = unsafe { &*parse };
    // https://drafts.csswg.org/cssom/#parse-a-css-declaration-block
    // 2. Let parsed declarations be a new empty list.
    let mut descriptors: Option<Arc<DescriptorBlockData>> = None;
    // 3. For each item declaration in declarations, follow these substeps:
    for item in &parsed.items {
        let ParsedRuleChild::Declarations(list) = item else {
            continue;
        };
        let Some(block) = &list.descriptor_block else {
            continue;
        };
        // 1. Let parsed declaration be the result of parsing declaration according to the appropriate CSS
        //    specifications, dropping parts that are said to be ignored. If the whole declaration is dropped, let
        //    parsed declaration be null.
        // 2. If parsed declaration is not null, append it to parsed declarations.
        // NB: Declaration-list items already own their parsed descriptors.
        if let Some(descriptors) = &mut descriptors {
            Arc::make_mut(descriptors)
                .descriptors
                .extend(block.descriptors.iter().cloned());
        } else {
            descriptors = Some(block.clone());
        }
    }
    // 4. Return parsed declarations.
    Box::into_raw(Box::new(FfiDescriptorBlock::new(descriptors.unwrap_or_default())))
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
    preserve_property_source_text: bool,
) -> *const ParsedStyleSheet {
    let Some(source) = (unsafe { source.units() }) else {
        return std::ptr::null_mut();
    };
    let Some(contexts) = (unsafe { crate::bytes_from_raw(contexts, context_count) }) else {
        return std::ptr::null_mut();
    };
    let Some(contexts) = rule_contexts(contexts) else {
        return std::ptr::null_mut();
    };
    Arc::into_raw(unsafe { parse_shared_block(source, contexts, parse_context, preserve_property_source_text) })
}

/// # Safety
/// The parse context and its referenced data must remain valid during parsing.
pub(crate) unsafe fn parse_shared_block(
    source: TokenizerInput<'_>,
    contexts: Vec<RuleContext>,
    parse_context: *const ParseContext,
    preserve_property_source_text: bool,
) -> Arc<ParsedStyleSheet> {
    let (mut parser, diagnostics) = Parser::from_source(source, contexts);
    let mut parse = SyntaxParseBuilder::new(parse_context, preserve_property_source_text);
    if parser.rule_context.contains(&RuleContext::AtFunction) {
        parse.declarations_kind = NestedDeclarationsKind::Function;
    }
    parse.diagnostics = diagnostics;
    let items = parser.consume_block_contents();
    parse.append_root_items(&items);
    Arc::new(parse.finish())
}

/// # Safety
/// `parse` must be null or an owned syntax parse handle on the current thread.
/// Each owned reference must only be freed once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_css_syntax_parse_free(parse: *const ParsedStyleSheet) {
    if !parse.is_null() {
        drop(unsafe { Arc::from_raw(parse) });
    }
}

#[cfg(test)]
mod tests {
    use super::{
        Declaration, FfiSyntaxDiagnosticCode, FontFeatureValuesRuleKind, PagePseudoClass, ParsedRuleChild,
        ParsedRuleKind, ParsedRulePrelude, ParsedStyleSheet, Rule, RuleContext, RuleOrDeclarations, SyntaxNode,
        SyntaxParseBuilder, at_rule_is_supported, at_rule_kind, collect_descriptors,
        consume_a_list_of_component_values, has_ignored_vendor_prefix, parse_block_contents, parse_font_feature_values,
        parse_keyframe_selectors, parse_page_selector_list, parse_rule, parse_rule_prelude, parse_stylesheet,
        rule_kind, token_diagnostics, tokenize_for_parser,
    };
    use crate::css::parser::value_parser::ParseContext;

    fn utf16(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }

    fn parse_test_rule_prelude(rule: &Rule) -> ParsedRulePrelude {
        parse_rule_prelude(rule, rule_kind(rule), None, &[], &|_, _| None)
    }

    pub(super) fn parse_context() -> ParseContext {
        ParseContext {
            in_quirks_mode: false,
            is_svg_presentation_attribute: false,
            is_substituted_value: false,
            contains_attr_tainted_values: false,
            is_ua_style_sheet: false,
            value_contexts: std::ptr::null(),
            value_context_count: 0,
            declared_namespaces: std::ptr::null(),
            document_url: std::ptr::null(),
            document_url_length: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            length_resolution_context: std::ptr::null(),
            random_function_index: std::ptr::null_mut(),
        }
    }

    fn parse_test_rule_prelude_with_context(rule: &Rule) -> ParsedRulePrelude {
        parse_rule_prelude(rule, rule_kind(rule), Some(&parse_context()), &[], &|_, _| None)
    }

    fn parse_test_stylesheet(source: &[u8]) -> ParsedStyleSheet {
        let rules = parse_stylesheet(source);
        let context = parse_context();
        let mut parse = SyntaxParseBuilder::new(&raw const context, false);
        parse.append_roots(&rules);
        parse.finish()
    }

    #[test]
    fn worker_diagnostics_visit_the_native_graph() {
        use super::{FfiDeclarationRejection, FfiSyntaxDeclaration, FfiSyntaxDiagnostic};
        use crate::css::ffi_support::FfiUtf16View;
        let parsed = std::thread::spawn(|| {
            let source = utf16("@未来 {} @media all { .目 { height: 13px; & {} 未知: 雪; width: 赤; } }");
            let parse = unsafe {
                std::sync::Arc::from_raw(super::rust_parse_css_stylesheet_syntax(
                    FfiUtf16View {
                        utf16: source.as_ptr(),
                        length: source.len(),
                        ..Default::default()
                    },
                    &parse_context(),
                ))
            };
            assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), 0);
            parse
        })
        .join()
        .unwrap();
        assert_eq!(parsed.declaration_errors.len(), 2);
        let names: Vec<_> = parsed
            .declaration_errors
            .iter()
            .map(|&index| {
                let declaration = &parsed.declarations[index];
                parsed.values[declaration.name_offset..declaration.name_offset + declaration.name_length].to_vec()
            })
            .collect();
        assert_eq!(names, [utf16("未知"), utf16("width")]);
        let parse = parsed;
        extern "C" fn diagnostic(values: *const u16, count: usize, diagnostic: &FfiSyntaxDiagnostic) {
            let values = unsafe { std::slice::from_raw_parts(values, count) };
            assert_eq!(diagnostic.code, FfiSyntaxDiagnosticCode::UnknownRule);
            assert_eq!(
                &values[diagnostic.primary_offset..diagnostic.primary_offset + diagnostic.primary_length],
                utf16("@未来")
            );
        }
        extern "C" fn declaration(values: *const u16, count: usize, declaration: &FfiSyntaxDeclaration) {
            let values = unsafe { std::slice::from_raw_parts(values, count) };
            let value = &values
                [declaration.value_source_offset..declaration.value_source_offset + declaration.value_source_length];
            match declaration.rejection {
                FfiDeclarationRejection::UnknownProperty => assert!(value.is_empty()),
                FfiDeclarationRejection::InvalidValue => assert_eq!(value, utf16("赤")),
                _ => panic!("Only rejected declarations should be visited"),
            }
        }
        assert_eq!(super::rust_css_syntax_root_count(&parse), 2);
        super::rust_css_syntax_visit_diagnostics(&parse, diagnostic);
        super::rust_css_syntax_visit_declaration_errors(&parse, declaration);
        let rules = unsafe { std::rc::Rc::from_raw(super::rust_css_syntax_native_rules(&*parse)) };
        assert_eq!(crate::css::rule::rust_rule_list_count(&rules), 1);
    }

    #[test]
    fn native_root_declaration_metadata_skips_nested_rules() {
        use super::FfiSyntaxDeclaration;
        use crate::css::ffi_support::FfiUtf16View;
        use std::ffi::c_void;
        let source = utf16("color: red; .child { height: 13px; } width: 雪;");
        let contexts = [RuleContext::Style as u8];
        let parse = unsafe {
            std::sync::Arc::from_raw(super::rust_parse_css_block_syntax(
                FfiUtf16View {
                    utf16: source.as_ptr(),
                    length: source.len(),
                    ..Default::default()
                },
                contexts.as_ptr(),
                contexts.len(),
                &parse_context(),
                true,
            ))
        };
        extern "C" fn visit(
            context: *mut c_void,
            values: *const u16,
            count: usize,
            declaration: &FfiSyntaxDeclaration,
        ) {
            let names = unsafe { &mut *context.cast::<Vec<Vec<u16>>>() };
            let values = unsafe { std::slice::from_raw_parts(values, count) };
            assert!(declaration.preserve_source_text);
            names.push(values[declaration.name_offset..declaration.name_offset + declaration.name_length].to_vec());
        }
        let mut names = Vec::<Vec<u16>>::new();
        super::rust_css_syntax_visit_root_declarations(&parse, (&raw mut names).cast(), visit);
        assert_eq!(names, [utf16("color"), utf16("width")]);
    }

    #[test]
    fn parse_result_owns_values_and_syntaxes_until_released() {
        let parse = parse_test_stylesheet(
            b"a { width: 13px } @property --size { syntax: '<length>'; inherits: false; initial-value: 7px }",
        );
        let values = parse
            .declarations
            .iter()
            .filter_map(|declaration| declaration.parsed_value.as_ref())
            .chain(
                parse
                    .rules
                    .iter()
                    .filter_map(|rule| rule.property_rule.as_ref()?.initial_value.as_ref()),
            )
            .filter(|value| matches!(value.as_ref(), crate::css::style_value::StyleValueData::Length { .. }))
            .map(std::sync::Arc::downgrade)
            .collect::<Vec<_>>();
        assert!(values.len() >= 2);
        let syntaxes = parse
            .rules
            .iter()
            .filter_map(|rule| rule.property_rule.as_ref().map(|rule| &rule.syntax))
            .map(std::sync::Arc::downgrade)
            .collect::<Vec<_>>();
        assert!(!syntaxes.is_empty());
        drop(parse);
        assert!(values.iter().all(|value| value.upgrade().is_none()));
        assert!(syntaxes.iter().all(|syntax| syntax.upgrade().is_none()));
    }

    #[test]
    fn consumers_can_retain_the_same_parsed_value_independently() {
        let parse = parse_test_stylesheet(b"a { width: 13px }");
        let pointer = std::sync::Arc::as_ptr(parse.declarations[0].parsed_value.as_ref().unwrap());
        let retain = || unsafe {
            std::sync::Arc::increment_strong_count(pointer);
            std::sync::Arc::from_raw(pointer)
        };
        let first = retain();
        let second = retain();
        drop(parse);
        assert!(std::sync::Arc::ptr_eq(&first, &second));
        drop(first);
        assert!(matches!(
            *second,
            crate::css::style_value::StyleValueData::Length { value: 13.0, .. }
        ));
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
        let mut parse = SyntaxParseBuilder::new(std::ptr::null(), false);
        let tokens =
            crate::css::css_tokenizer::tokenize_for_parser(&utf16("a { color: \"bad\n; background: url(foo\"bar) }"));
        parse.diagnostics = token_diagnostics(&tokens);
        assert_eq!(parse.diagnostics.len(), 2);
        assert_eq!(parse.diagnostics[0].code, FfiSyntaxDiagnosticCode::BadString);
        assert_eq!(parse.diagnostics[0].start_line, 0);
        assert!(parse.diagnostics[0].end_column > parse.diagnostics[0].start_column);
        assert_eq!(parse.diagnostics[1].code, FfiSyntaxDiagnosticCode::BadUrl);
        assert!(parse.diagnostics[1].end_column > parse.diagnostics[1].start_column);
    }

    #[test]
    fn classifies_structurally_invalid_and_misplaced_rules() {
        let parse = parse_test_stylesheet(
            b"undeclared|element {} @font-face serif {} @import url('data:text/css,'); \
              @namespace known 'urn:known'; known|element {}",
        );
        let root_kinds = parse.rules.iter().map(|rule| rule.rule_kind).collect::<Vec<_>>();
        assert_eq!(
            root_kinds,
            vec![
                ParsedRuleKind::Invalid,
                ParsedRuleKind::Invalid,
                ParsedRuleKind::Import,
                ParsedRuleKind::Namespace,
                ParsedRuleKind::Qualified,
            ]
        );
        assert!(
            parse
                .diagnostics
                .iter()
                .any(|diagnostic| diagnostic.code == FfiSyntaxDiagnosticCode::StyleSelectorsInvalid)
        );
        assert!(
            parse
                .diagnostics
                .iter()
                .any(|diagnostic| diagnostic.code == FfiSyntaxDiagnosticCode::FontFacePreludeNotAllowed)
        );

        let parse = parse_test_stylesheet(b"element {} @import url('data:text/css,'); @namespace late 'urn:late';");
        assert_eq!(parse.rules[1].rule_kind, ParsedRuleKind::Invalid);
        assert_eq!(parse.rules[2].rule_kind, ParsedRuleKind::Invalid);
        assert!(
            parse
                .diagnostics
                .iter()
                .any(|diagnostic| diagnostic.code == FfiSyntaxDiagnosticCode::MisplacedImport)
        );
        assert!(
            parse
                .diagnostics
                .iter()
                .any(|diagnostic| diagnostic.code == FfiSyntaxDiagnosticCode::MisplacedNamespace)
        );
    }

    #[test]
    fn stray_feature_value_rules_do_not_close_ordering_windows() {
        // A feature value rule is only valid inside @font-feature-values. Anywhere else it is
        // dropped, so it must not count as a "valid at-rule" for @import and @namespace ordering.
        let parse = parse_test_stylesheet(
            b"@styleset {} @import url('data:text/css,'); @namespace known 'urn:known'; known|element {}",
        );
        let root_kinds = parse.rules.iter().map(|rule| rule.rule_kind).collect::<Vec<_>>();
        assert_eq!(
            root_kinds,
            vec![
                ParsedRuleKind::Invalid,
                ParsedRuleKind::Import,
                ParsedRuleKind::Namespace,
                ParsedRuleKind::Qualified,
            ]
        );

        let parse = parse_test_stylesheet(b"@media all { @styleset {} } @font-feature-values Family { @styleset {} }");
        let ParsedRuleChild::Rule(media_child) = &parse.rules[0].children[0] else {
            panic!("expected rule")
        };
        assert_eq!(media_child.rule_kind, ParsedRuleKind::Invalid);
        let ParsedRuleChild::Rule(font_feature_values_child) = &parse.rules[1].children[0] else {
            panic!("expected rule")
        };
        assert_eq!(
            font_feature_values_child.rule_kind,
            ParsedRuleKind::FontFeatureValuesRule
        );
    }

    #[test]
    fn validates_nested_rule_contexts_without_flattening_them() {
        let parse = parse_test_stylesheet(b"a { @layer direct; } @scope { @layer scoped; }");
        let layer_rules = parse
            .rules
            .iter()
            .flat_map(|rule| rule.children.iter())
            .filter_map(|child| match child {
                ParsedRuleChild::Rule(rule) => Some(rule),
                _ => None,
            })
            .filter(|rule| matches!(rule.rule_kind, ParsedRuleKind::Invalid | ParsedRuleKind::Layer))
            .map(|rule| rule.rule_kind)
            .collect::<Vec<_>>();
        assert_eq!(layer_rules, vec![ParsedRuleKind::Invalid, ParsedRuleKind::Layer]);
        assert!(
            parse
                .diagnostics
                .iter()
                .any(|diagnostic| diagnostic.code == FfiSyntaxDiagnosticCode::InvalidRuleLocation)
        );
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
        let rules = parse_stylesheet(b"@top-left { color: red }");
        let [Rule::At(rule)] = rules.as_slice() else {
            panic!("expected margin at-rule");
        };
        assert!(!rule.valid_in_context);
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
    fn rejects_invalid_selector_preludes() {
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
        let rules = parse_stylesheet(b"a {} :heading(1.0) {} a { > b {} @scope (> .start) to (> .end) {} }");
        let mut parse = SyntaxParseBuilder::new(std::ptr::null(), false);
        parse.append_roots(&rules);

        assert!(parse.rules[0].selector_list.is_some());
        assert!(parse.rules[1].selector_list.is_none());

        assert!(parse.rules[2].selector_list.is_some());
        let [ParsedRuleChild::Rule(child), ParsedRuleChild::Rule(scope_rule)] = &*parse.rules[2].children else {
            panic!("expected nested style and scope rules")
        };
        assert!(child.selector_list.is_some());
        let selectors = scope_rule.scope_selectors.as_ref().unwrap();
        assert!(selectors.start.is_some());
        assert!(selectors.end.is_some());

        let rules = parse_stylesheet(b"@media all { > b {} @scope (> .start) {} }");
        let mut parse = SyntaxParseBuilder::new(std::ptr::null(), false);
        parse.append_roots(&rules);
        let [ParsedRuleChild::Rule(group_child), ParsedRuleChild::Rule(scope_rule)] = &*parse.rules[0].children else {
            panic!("expected nested style and scope rules")
        };
        assert!(group_child.selector_list.is_none());
        assert_eq!(scope_rule.rule_kind, ParsedRuleKind::Invalid);
        assert!(scope_rule.scope_selectors.is_none());
    }

    #[test]
    fn classifies_rule_kinds() {
        let expected = [
            ("container", ParsedRuleKind::Container),
            ("counter-style", ParsedRuleKind::CounterStyle),
            ("font-face", ParsedRuleKind::FontFace),
            ("font-feature-values", ParsedRuleKind::FontFeatureValues),
            ("annotation", ParsedRuleKind::FontFeatureValuesRule),
            ("function", ParsedRuleKind::Function),
            ("import", ParsedRuleKind::Import),
            ("keyframes", ParsedRuleKind::Keyframes),
            ("-webkit-keyframes", ParsedRuleKind::Keyframes),
            ("layer", ParsedRuleKind::Layer),
            ("top-left", ParsedRuleKind::Margin),
            ("media", ParsedRuleKind::Media),
            ("namespace", ParsedRuleKind::Namespace),
            ("page", ParsedRuleKind::Page),
            ("property", ParsedRuleKind::Property),
            ("scope", ParsedRuleKind::Scope),
            ("supports", ParsedRuleKind::Supports),
            ("unknown", ParsedRuleKind::Unknown),
            ("-webkit-unknown", ParsedRuleKind::IgnoredVendor),
            ("-libweb-unknown", ParsedRuleKind::Unknown),
            ("-unknown", ParsedRuleKind::Unknown),
            ("--unknown", ParsedRuleKind::Unknown),
        ];
        for (name, expected) in expected {
            assert_eq!(at_rule_kind(&utf16(name)), expected, "{name}");
            assert_eq!(
                at_rule_is_supported(&utf16(name)),
                !matches!(expected, ParsedRuleKind::Unknown | ParsedRuleKind::IgnoredVendor),
                "{name}"
            );
        }
        assert!(!at_rule_is_supported(&utf16("charset")));
        assert!(has_ignored_vendor_prefix(b"-webkit-unknown".into()));
        assert!(!has_ignored_vendor_prefix(b"-libweb-unknown".into()));
        assert_eq!(rule_kind(&parse_stylesheet(b"a {}")[0]), ParsedRuleKind::Qualified);
    }

    #[test]
    fn parses_query_preludes_from_component_values() {
        let rules = parse_stylesheet(
            b"@media screen and (width > 1px) {} @supports (display: grid) {} @container card (width > 1px), style(--theme: dark) {} @supports {}",
        );
        let mut parse = SyntaxParseBuilder::new(std::ptr::null(), false);
        parse.append_roots(&rules);

        let media = &parse.rules[0];
        assert_eq!(media.rule_kind, ParsedRuleKind::Media);
        assert_eq!(media.media_list.as_ref().unwrap().queries.len(), 1);

        let supports = &parse.rules[1];
        assert_eq!(supports.rule_kind, ParsedRuleKind::Supports);
        assert!(supports.supports_condition.is_some());

        let container = &parse.rules[2];
        assert_eq!(container.rule_kind, ParsedRuleKind::Container);
        let conditions = &container.container_conditions.as_ref().unwrap().conditions;
        assert_eq!(conditions.len(), 2);
        assert!(conditions[0].name.is_some());
        assert!(conditions[0].query.is_some());
        assert!(conditions[1].name.is_none());
        assert!(conditions[1].query.is_some());

        let invalid_supports = &parse.rules[3];
        assert_eq!(invalid_supports.rule_kind, ParsedRuleKind::Invalid);
        assert!(invalid_supports.supports_condition.is_none());
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
            vec![PagePseudoClass::Left, PagePseudoClass::First]
        );
        assert_eq!(selectors[1].name, None);
        assert_eq!(selectors[1].pseudo_classes, vec![PagePseudoClass::Blank]);
        let ParsedRulePrelude::Scope { start, end } = parse_test_rule_prelude(&rules[5]) else {
            panic!("expected scope selectors");
        };
        assert!(start.is_some());
        assert!(end.is_some());
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
            parse_rule_prelude(&rules[0], rule_kind(&rules[0]), Some(&context), &[], &|_, _| None),
            ParsedRulePrelude::Name(utf16("decimal").into_boxed_slice().into())
        );
    }

    #[test]
    fn expands_and_deduplicates_descriptors_in_last_valid_order() {
        use crate::css::descriptor_metadata::descriptor_metadata;

        let sheet = parse_test_stylesheet(
            b"@page { margin: 1px 2px 3px 4px; @top-left { content: 'page'; } margin-right: 5px; size: a4; size: invalid; }",
        );
        let descriptors = &sheet.rules[0].descriptor_block.as_ref().unwrap().descriptors;
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
        let descriptors = collect_descriptors(declarations, &parse_context());
        assert_eq!(descriptors.len(), 2);
        assert_eq!(descriptors[0].name.units(), utf16("--second"));
        assert_eq!(descriptors[1].name.units(), utf16("--first"));
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
            ParsedRulePrelude::FontFeatureValuesRule(FontFeatureValuesRuleKind::HistoricalForms)
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

    #[test]
    fn parses_page_selector_lists_directly() {
        let selectors = parse_page_selector_list(b"invoice:left:first, :blank".as_slice()).unwrap();
        assert_eq!(selectors.len(), 2);
        assert_eq!(selectors[0].name.as_deref(), Some(utf16("invoice").as_slice()));
        assert_eq!(
            selectors[0].pseudo_classes,
            vec![PagePseudoClass::Left, PagePseudoClass::First]
        );
        assert_eq!(selectors[1].name, None);
        assert_eq!(selectors[1].pseudo_classes, vec![PagePseudoClass::Blank]);

        assert_eq!(parse_page_selector_list(b"".as_slice()), Some(Vec::new()));
        let escaped = parse_page_selector_list(br"invo\69 ce:first".as_slice()).unwrap();
        assert_eq!(escaped[0].name.as_deref(), Some(utf16("invoice").as_slice()));

        for source in [
            b"named :first".as_slice(),
            b":unknown",
            b"named trailing",
            b"named {}",
            b"named; trailing",
            b"named,",
            b",named",
        ] {
            assert_eq!(
                parse_page_selector_list(source),
                None,
                "{}",
                String::from_utf8_lossy(source)
            );
        }
    }
}
