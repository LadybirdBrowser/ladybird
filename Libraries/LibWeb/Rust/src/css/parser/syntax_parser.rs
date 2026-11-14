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
    CssNumberType, ParserString, ParserTokenKind, SourcePosition, TokenizerInput, tokenize_for_parser,
};
use crate::css::declaration_block::{
    DeclarationBlock, DeclarationBlockBuilder, DeclaredProperty, SharedDeclarationBlock,
};
use crate::css::descriptor_block::{DescriptorBlockData, DescriptorData, FfiDescriptorBlock};
use crate::css::descriptor_metadata::{CUSTOM_DESCRIPTOR_ID, descriptor_longhands};
use crate::css::ffi_support::{ArcRef, FfiUtf16View, OwnedArc};
pub use crate::css::font_feature_values::FontFeatureValuesRuleKind;
use crate::css::font_feature_values::{FontFeatureValuesData, FontFeatureValuesRule};
use crate::css::function_signature::{FunctionParameterData, FunctionSignature};
use crate::css::import_rule::ImportRuleData;
use crate::css::keyframes::{FfiKeyframe, KeyframeData};
use crate::css::layer_names::LayerNames;
use crate::css::media_list::{MediaList, MediaListData};
use crate::css::namespace_rule::NamespaceRuleData;
use crate::css::parser::arbitrary_substitution::{
    ArbitrarySubstitutionFunction, arbitrary_substitution_function, arguments_are_valid, declaration_value_is_valid,
};
use crate::css::parser::component_value::{
    ComponentKind, ComponentSerializationMode, ComponentValue, consume_a_list_of_component_values, trim_whitespace,
};
use crate::css::parser::descriptor_parser::parse_descriptor;
use crate::css::parser::query_parser::{
    ContainerCondition, Expression, FfiQueryHandle, MediaQuery, QueryKind, declared_namespaces_from_context,
    expression_query_handle, parse_container_condition_list_from_component_values,
    parse_media_query_list_from_component_values, parse_supports_condition_from_component_values,
    parse_supports_declaration_from_component_values, resolve_query_feature, supports_feature_matches,
};
use crate::css::parser::rows::Rows;
use crate::css::parser::sheet_source_text::SheetSourceText;
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
use crate::css::selector_arena::{NestingTarget, RuleOrdinal};
use crate::css::selector_parser::{
    RustParsedSelectorList, SelectorType, StyleNestingParent, parse_selector_list_from_component_values,
    parse_selector_list_of_rows,
};
use crate::css::serialize::TextSink;
use crate::css::serialize::serialize_component_values_to_utf16;
use crate::css::style_compute::value_is_computationally_independent;
use crate::css::style_rule::StyleRule;
use crate::css::style_value::{RetainedRequestUrlModifierList, RetainedString, StyleValueBox, StyleValueData};
use crate::css::style_value::{ShorthandPayload, UrlPayload};
use crate::css::style_value_arc::StyleValueArc;
use std::borrow::Cow;
use std::cell::{Cell, RefCell};
use std::ffi::c_void;
use std::fmt;
use std::rc::Rc;
use std::sync::Arc;

mod shared_rules;
mod structure;

#[cfg(test)]
mod ownership_tests;
pub(crate) use shared_rules::{ParsedRuleList, SharedRule};
pub(crate) use structure::DeclarationValue;
use structure::{Parser, source_of};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
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
    AtPositionTry,
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
    PositionTry,
    Property,
    Scope,
    Supports,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum Rule {
    At(AtRule),
    Qualified(QualifiedRule),
}

/// What the rule around a rule is, for the diagnostics that name it.
#[derive(Clone, Debug, PartialEq)]
pub(crate) enum OuterRuleName {
    Context(RuleContext),
    /// The name of an at-rule, after its `@`.
    At(Arc<[u16]>),
}

impl OuterRuleName {
    fn name(&self) -> Option<ParserString> {
        match self {
            Self::Context(context) => context_rule_name(*context),
            Self::At(name) => Some(at_rule_name(name)),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct AtRule {
    pub name: ParserString,
    pub prelude: Vec<ComponentValue>,
    pub has_block: bool,
    nesting_parent: StyleNestingParent,
    valid_in_context: bool,
    outer_rule_name: Option<OuterRuleName>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct QualifiedRule {
    pub prelude: structure::Prelude,
    pub prelude_is_selector: bool,
    nesting_parent: StyleNestingParent,
    valid_in_context: bool,
    outer_rule_name: Option<OuterRuleName>,
    pub source_position: Option<SourcePosition>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Declaration {
    pub value: DeclarationValue,
    pub important: bool,
    pub source_position: SourcePosition,
    pub is_property: bool,
    pub rule_context: RuleContext,
}

impl Declaration {
    /// The name of the declaration, which is not its text where that has escapes.
    pub(crate) fn name(&self) -> crate::css::parser::rows::Name {
        self.value.name()
    }
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
pub(crate) struct ParsedPageSelector {
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
struct ParsedStyleValue(StyleValueArc);

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
pub(crate) fn parse_page_selector_list<'a>(source: impl Into<TokenizerInput<'a>>) -> Option<Vec<ParsedPageSelector>> {
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
    pub(crate) fn new(parsed_selectors: Vec<ParsedPageSelector>) -> Self {
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
            Some(start.adapt_for_nesting(nesting_parent).into_list(nesting_parent))
        }
        None => None,
    };
    let end = match end {
        Some(end) => {
            let Some(end) = parse_selector(end, SelectorType::Relative) else {
                return ParsedRulePrelude::Invalid;
            };
            Some(end.into_list(StyleNestingParent::None))
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
    Some(selectors.into_list(StyleNestingParent::None))
}

fn parse_import_url(context: &ParseContext, value: &ComponentValue) -> Option<ParsedStyleValue> {
    let url = match &value.kind {
        ComponentKind::Token(ParserTokenKind::String(url)) => StyleValueData::Url(StyleValueBox::new(UrlPayload {
            url: RetainedString::from_utf16(url)?,
            url_type: 0,
            modifiers: RetainedRequestUrlModifierList::from_vec(Vec::new()),
        })),
        ComponentKind::Token(ParserTokenKind::Url(_)) => parse_url_value(context, value)?,
        ComponentKind::Function { name, .. } if equals_ascii_case_insensitive(name, b"url") => {
            parse_url_value(context, value)?
        }
        _ => return None,
    };
    Some(ParsedStyleValue(StyleValueArc::new(url)))
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
        StyleValueData::Unresolved(unresolved)
            if unresolved.presence().attr
                || unresolved.presence().dashed_function
                || unresolved.presence().env
                || unresolved.presence().if_
                || unresolved.presence().inherit
                || unresolved.presence().var
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
    parse_with_syntax(context, &source, syntax).map(|value| ParsedStyleValue(StyleValueArc::new(value)))
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

fn initial_value_contains_variable_reference(values: &[ComponentValue]) -> Option<bool> {
    let mut contains_reference = false;
    for value in values {
        let nested_values = match &value.kind {
            ComponentKind::Function { name, values } => {
                match arbitrary_substitution_function(name.as_ref()) {
                    Some(ArbitrarySubstitutionFunction::Var) => {
                        let (name, _) = crate::css::style_value::split_var_arguments(values)?;
                        if name.len() < 3 {
                            return None;
                        }
                        contains_reference = true;
                    }
                    // NB: Other substitutions retain their descriptor handling, but cannot hide a var() reference.
                    Some(function) if !arguments_are_valid(function, values) => return None,
                    Some(_) | None => {}
                }
                values.as_ref()
            }
            ComponentKind::SimpleBlock { values, .. } => values.as_ref(),
            ComponentKind::Token(_) => continue,
        };
        contains_reference |= initial_value_contains_variable_reference(nested_values)?;
    }
    Some(contains_reference)
}

fn parse_property_prelude(rule: &AtRule, declarations: &[Declaration], context: &ParseContext) -> ParsedRulePrelude {
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
    let mut initial_value_is_invalid = false;
    for declaration in declarations {
        let name = declaration.name();
        let name = name.as_slice();
        let source = declaration.value.text();
        let declaration_values = declaration.value.component_values();
        if parse_descriptor(context, 2, name, &declaration_values, source.as_ref()).is_none() {
            // NB: A var() reference matches the descriptor's <declaration-value>? grammar, so it replaces an earlier
            //     initial value. It is not computationally independent, so it makes the rule invalid.
            if equals_ascii_case_insensitive(name, b"initial-value")
                && !declaration.important
                && declaration_value_is_valid(&declaration_values)
                && initial_value_contains_variable_reference(&declaration_values) == Some(true)
            {
                initial_value_is_invalid = true;
            }
            continue;
        }
        if equals_ascii_case_insensitive(name, b"syntax") {
            let values = non_whitespace(&declaration_values);
            syntax_source = values
                .as_slice()
                .first()
                .and_then(|value| value.string())
                .filter(|_| values.len() == 1)
                .map(|source| source.to_vec().into_boxed_slice().into());
        } else if equals_ascii_case_insensitive(name, b"inherits") {
            let values = non_whitespace(&declaration_values);
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
        } else if equals_ascii_case_insensitive(name, b"initial-value") {
            initial_value_is_invalid = false;
            initial_value_source = Some(serialize_component_values_to_utf16(
                &declaration_values,
                ComponentSerializationMode::Retokenize,
            ));
        }
    }

    if initial_value_is_invalid {
        return ParsedRulePrelude::Invalid;
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
        ParsedStyleValue(StyleValueArc::new(
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

/// Parses the prelude of `rule`. `declarations` are the declarations in the lists of declarations that are children
/// of the rule, which the prelude of an `@property` rule is made from.
fn parse_rule_prelude<R>(
    rule: &Rule,
    declarations: &[Declaration],
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
            parse_keyframe_selectors(&rule.prelude.component_values())
        }
        (Rule::Qualified(_), ParsedRuleKind::Qualified) => ParsedRulePrelude::Unparsed,
        (Rule::At(rule), ParsedRuleKind::Layer) => parse_layer_prelude(&rule.prelude, rule.has_block),
        (Rule::At(rule), ParsedRuleKind::Property) => parse_context
            .map(|context| parse_property_prelude(rule, declarations, context))
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
        // https://drafts.csswg.org/css-anchor-position-1/#fallback-rule
        // @position-try <dashed-ident> { <declaration-list> }
        (Rule::At(rule), ParsedRuleKind::PositionTry) => parse_single_name(&rule.prelude, |name| {
            name.starts_with(&[u16::from(b'-'), u16::from(b'-')]) && name.len() > 2
        }),
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
        RuleContext::AtPositionTry => b"@position-try",
    };
    Some(utf16_from_ascii(name))
}

fn at_rule_name(name: &[u16]) -> ParserString {
    let mut result = Vec::with_capacity(name.len() + 1);
    result.push(u16::from(b'@'));
    result.extend_from_slice(name);
    result.into_boxed_slice().into()
}

// https://drafts.csswg.org/css-anchor-position-1/#accepted-position-try-properties
fn is_accepted_position_try_property(property: u16) -> bool {
    matches!(
        property,
        // inset properties
        property_id::INSET
            | property_id::INSET_BLOCK
            | property_id::INSET_BLOCK_START
            | property_id::INSET_BLOCK_END
            | property_id::INSET_INLINE
            | property_id::INSET_INLINE_START
            | property_id::INSET_INLINE_END
            | property_id::TOP
            | property_id::RIGHT
            | property_id::BOTTOM
            | property_id::LEFT
            // margin properties
            | property_id::MARGIN
            | property_id::MARGIN_BLOCK
            | property_id::MARGIN_BLOCK_START
            | property_id::MARGIN_BLOCK_END
            | property_id::MARGIN_INLINE
            | property_id::MARGIN_INLINE_START
            | property_id::MARGIN_INLINE_END
            | property_id::MARGIN_TOP
            | property_id::MARGIN_RIGHT
            | property_id::MARGIN_BOTTOM
            | property_id::MARGIN_LEFT
            // sizing properties
            | property_id::WIDTH
            | property_id::HEIGHT
            | property_id::MIN_WIDTH
            | property_id::MIN_HEIGHT
            | property_id::MAX_WIDTH
            | property_id::MAX_HEIGHT
            | property_id::BLOCK_SIZE
            | property_id::INLINE_SIZE
            | property_id::MIN_BLOCK_SIZE
            | property_id::MIN_INLINE_SIZE
            | property_id::MAX_BLOCK_SIZE
            | property_id::MAX_INLINE_SIZE
            // self-alignment properties
            | property_id::ALIGN_SELF
            | property_id::JUSTIFY_SELF
            | property_id::PLACE_SELF
            // position-anchor and position-area
            | property_id::POSITION_ANCHOR
            | property_id::POSITION_AREA
    )
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

/// What the rule is nested in, as `&` sees it.
fn rule_nesting_parent(rule: &Rule) -> StyleNestingParent {
    match rule {
        Rule::At(rule) => rule.nesting_parent,
        Rule::Qualified(rule) => rule.nesting_parent,
    }
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
    } else if equals_ascii_case_insensitive(name, b"position-try") {
        ParsedRuleKind::PositionTry
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
    } else if equals_ascii_case_insensitive(name, b"position-try") {
        RuleContext::AtPositionTry
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

pub(crate) fn supports_declaration_matches(context: &ParseContext, source: &[u16]) -> bool {
    let source = source_of(source.into());
    /// Takes the declarations of a source, and counts its lists of declarations and its rules.
    #[derive(Default)]
    struct Declarations {
        declarations: Vec<Declaration>,
        list_count: usize,
        rule_count: usize,
        is_in_list: bool,
    }
    impl structure::Sink for Declarations {
        fn begin_rule(&mut self, _: Rule, _: Option<&Rows>) {
            self.rule_count += 1;
        }
        fn end_rule(&mut self) {}
        fn declaration(&mut self, declaration: Declaration) {
            self.list_count += usize::from(!self.is_in_list);
            self.is_in_list = true;
            self.declarations.push(declaration);
        }
        fn end_declarations(&mut self) {
            self.is_in_list = false;
        }
    }
    let mut found = Declarations::default();
    Parser::new(&source, vec![RuleContext::SupportsCondition]).parse_block_contents(&mut found);
    if found.rule_count != 0 || found.list_count != 1 {
        return false;
    }
    let [declaration] = found.declarations.as_slice() else {
        return false;
    };
    let Some(property_id) = property_id_from_name(declaration.name().as_ref()) else {
        return false;
    };

    let source = declaration.value.text();
    let values = declaration.value.component_values();
    let property_context = FfiValueParsingContext {
        kind: FfiValueParsingContextKind::Property,
        value: property_id,
        secondary_value: 0,
        name: Default::default(),
    };
    let mut value_context = *context;
    value_context.is_ua_style_sheet = false;
    value_context.value_contexts = ak::Span::new(std::slice::from_ref(&property_context));
    value_context.length_resolution_context = None;
    let outcome = parse_css_value_with_utf16_source(&value_context, property_id, &values, source.as_ref());
    if let Some(random_function_index) = context.random_function_index {
        random_function_index.set(0);
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
    source_position: Option<(u32, u32)>,
    declaration_block: Option<SharedDeclarationBlock>,
    descriptor_block: Option<Arc<DescriptorBlockData>>,
}

enum ParsedRuleChild {
    /// A rule, by its index among the rules of the sheet.
    Rule(u32),
    /// Boxed, as most children are rules, which would otherwise each pay for a declaration list.
    Declarations(Box<ParsedDeclarationList>),
}

fn declaration_block_from_blocks<'a>(
    blocks: impl Iterator<Item = &'a SharedDeclarationBlock>,
) -> SharedDeclarationBlock {
    // https://drafts.csswg.org/cssom/#parse-a-css-declaration-block
    // 2. Let parsed declarations be a new empty list.
    // NB: A single item's block is shared as is; a builder is made only once a second item appends.
    let mut first: Option<&SharedDeclarationBlock> = None;
    let mut declarations: Option<DeclarationBlockBuilder> = None;
    // 3. For each item declaration in declarations, follow these substeps:
    for block in blocks {
        // 1. Let parsed declaration be the result of parsing declaration according to the appropriate CSS
        //    specifications, dropping parts that are said to be ignored. If the whole declaration is dropped, let
        //    parsed declaration be null.
        // 2. If parsed declaration is not null, append it to parsed declarations.
        // NB: Each declaration-list item already owns its parsed, normalized properties.
        if let Some(declarations) = &mut declarations {
            declarations.append_block(block);
        } else if let Some(first) = first {
            let mut builder = first.to_builder();
            builder.append_block(block);
            declarations = Some(builder);
        } else {
            first = Some(block);
        }
    }
    // 4. Return parsed declarations.
    match (declarations, first) {
        (Some(declarations), _) => declarations.finish(),
        (None, Some(first)) => first.clone(),
        (None, None) => SharedDeclarationBlock::default(),
    }
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
    PositionTryMustBeBlock,
    PositionTryInvalidName,
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
    parsed_value: Option<StyleValueArc>,
    font_feature_values_start: usize,
    font_feature_value_count: usize,
}

struct ParsedRule {
    local_identity: u32,
    is_qualified: bool,
    rule_kind: ParsedRuleKind,
    has_block: bool,
    has_source_position: bool,
    start_line: u32,
    start_column: u32,
    /// Where the children of the rule start among the children of the sheet, and how many there are. A rule without
    /// child rules or declarations, as most style rules are, has none there.
    first_child: u32,
    child_count: u32,
    /// Where the positions among its children of the rules the CSSOM sees start among the native children of the
    /// sheet, and how many there are.
    first_native_child: u32,
    native_child_count: u32,
    payload: ParsedRulePayload,
}

// Most of a sheet's parse graph is its rules.
const _: () = assert!(size_of::<ParsedRulePayload>() == 16);
const _: () = assert!(size_of::<ParsedRule>() == 48);

/// What a rule holds of its own kind: a style rule its selectors and declarations, an at-rule its
/// condition, name or definition. Most of a sheet's rules are style rules, so each rule holds the
/// payload of its own kind alone, rather than an empty slot for the payload of every other kind.
enum ParsedRulePayload {
    None,
    Style {
        /// Where the rule's selector list starts in the selector arena of the sheet, which holds the arena once for all
        /// its rules. None for a rule whose prelude is not a selector list.
        selectors: Option<RuleSelectors>,
        declaration_block: SharedDeclarationBlock,
    },
    Keyframe(Arc<KeyframeData>),
    Import(Box<ImportPayload>),
    Media(Arc<MediaListData>),
    Supports(Arc<FfiQueryHandle>),
    Container(Arc<ContainerConditionsData>),
    Scope(Arc<ScopeSelectors>),
    Layer(Arc<LayerNames>),
    Page(Box<PagePayload>),
    FontFace(Arc<DescriptorBlockData>),
    FontFeatureValues(Arc<FontFeatureValuesData>),
    /// A block of `@font-feature-values`, such as `@swash`, which its parent reads.
    FontFeatureValuesRule(FontFeatureValuesRuleKind),
    Keyframes(CssString),
    /// A margin rule, whose name is in ASCII lowercase.
    Margin(Box<NamedBlockPayload>),
    PositionTry(Box<NamedBlockPayload>),
    CounterStyle(Arc<CounterStyleData>),
    Namespace(Arc<NamespaceRuleData>),
    Property(Arc<PropertyRuleData>),
    Function(Arc<FunctionSignature>),
}

/// Where a style rule's selector list starts in the selector arena of its sheet.
#[derive(Clone, Copy)]
struct RuleSelectors(Option<crate::css::selector_arena::ListStart>);

// NB: The payloads of rare rules that hold two things are boxed, so that they do not make every rule larger.
struct ImportPayload {
    import_rule: Arc<ImportRuleData>,
    media_list: Arc<MediaListData>,
}

struct PagePayload {
    selectors: Arc<PageSelectorList>,
    descriptors: Arc<DescriptorBlockData>,
}

struct NamedBlockPayload {
    name: CssString,
    declaration_block: SharedDeclarationBlock,
}

impl ParsedRule {
    /// Where the children of the rule are among the children of the sheet.
    fn child_range(&self) -> std::ops::Range<usize> {
        let first = self.first_child as usize;
        first..first + self.child_count as usize
    }

    /// Where the positions of the rule's native children are among the native children of the sheet.
    fn native_child_range(&self) -> std::ops::Range<usize> {
        let first = self.first_native_child as usize;
        first..first + self.native_child_count as usize
    }

    fn source_position(&self) -> Option<(usize, usize)> {
        self.has_source_position
            .then_some((self.start_line as usize, self.start_column as usize))
    }

    fn selectors(&self) -> Option<RuleSelectors> {
        match &self.payload {
            ParsedRulePayload::Style { selectors, .. } => *selectors,
            _ => None,
        }
    }

    fn declaration_block(&self) -> Option<&SharedDeclarationBlock> {
        match &self.payload {
            ParsedRulePayload::Style { declaration_block, .. } => Some(declaration_block),
            ParsedRulePayload::Margin(payload) | ParsedRulePayload::PositionTry(payload) => {
                Some(&payload.declaration_block)
            }
            ParsedRulePayload::Keyframe(keyframe) => Some(&keyframe.declarations),
            _ => None,
        }
    }

    fn keyframe(&self) -> Option<&Arc<KeyframeData>> {
        match &self.payload {
            ParsedRulePayload::Keyframe(keyframe) => Some(keyframe),
            _ => None,
        }
    }

    fn import_rule(&self) -> Option<&Arc<ImportRuleData>> {
        match &self.payload {
            ParsedRulePayload::Import(payload) => Some(&payload.import_rule),
            _ => None,
        }
    }

    fn media_list(&self) -> Option<&Arc<MediaListData>> {
        match &self.payload {
            ParsedRulePayload::Import(payload) => Some(&payload.media_list),
            ParsedRulePayload::Media(media_list) => Some(media_list),
            _ => None,
        }
    }

    fn supports_condition(&self) -> Option<&Arc<FfiQueryHandle>> {
        match &self.payload {
            ParsedRulePayload::Supports(condition) => Some(condition),
            _ => None,
        }
    }

    fn container_conditions(&self) -> Option<&Arc<ContainerConditionsData>> {
        match &self.payload {
            ParsedRulePayload::Container(conditions) => Some(conditions),
            _ => None,
        }
    }

    fn scope_selectors(&self) -> Option<&Arc<ScopeSelectors>> {
        match &self.payload {
            ParsedRulePayload::Scope(selectors) => Some(selectors),
            ParsedRulePayload::Import(payload) => payload.import_rule.scope.as_ref(),
            _ => None,
        }
    }

    fn layer_names(&self) -> Option<&Arc<LayerNames>> {
        match &self.payload {
            ParsedRulePayload::Layer(names) => Some(names),
            _ => None,
        }
    }

    fn page_selector_list(&self) -> Option<&Arc<PageSelectorList>> {
        match &self.payload {
            ParsedRulePayload::Page(payload) => Some(&payload.selectors),
            _ => None,
        }
    }

    /// The descriptors of a rule that holds them: `@font-face`, `@page` and `@counter-style`.
    fn descriptor_block(&self) -> Option<&Arc<DescriptorBlockData>> {
        match &self.payload {
            ParsedRulePayload::FontFace(descriptors) => Some(descriptors),
            ParsedRulePayload::Page(payload) => Some(&payload.descriptors),
            ParsedRulePayload::CounterStyle(counter_style) => Some(&counter_style.descriptors),
            _ => None,
        }
    }

    fn margin_name(&self) -> Option<&CssString> {
        match &self.payload {
            ParsedRulePayload::Margin(payload) => Some(&payload.name),
            _ => None,
        }
    }

    fn font_feature_values(&self) -> Option<&Arc<FontFeatureValuesData>> {
        match &self.payload {
            ParsedRulePayload::FontFeatureValues(values) => Some(values),
            _ => None,
        }
    }

    fn keyframes_name(&self) -> Option<&CssString> {
        match &self.payload {
            ParsedRulePayload::Keyframes(name) => Some(name),
            _ => None,
        }
    }

    fn position_try_name(&self) -> Option<&CssString> {
        match &self.payload {
            ParsedRulePayload::PositionTry(payload) => Some(&payload.name),
            _ => None,
        }
    }

    fn counter_style(&self) -> Option<&Arc<CounterStyleData>> {
        match &self.payload {
            ParsedRulePayload::CounterStyle(counter_style) => Some(counter_style),
            _ => None,
        }
    }

    fn namespace_rule(&self) -> Option<&Arc<NamespaceRuleData>> {
        match &self.payload {
            ParsedRulePayload::Namespace(namespace) => Some(namespace),
            _ => None,
        }
    }

    fn property_rule(&self) -> Option<&Arc<PropertyRuleData>> {
        match &self.payload {
            ParsedRulePayload::Property(property) => Some(property),
            _ => None,
        }
    }

    fn function_signature(&self) -> Option<&Arc<FunctionSignature>> {
        match &self.payload {
            ParsedRulePayload::Function(signature) => Some(signature),
            _ => None,
        }
    }
}

/// A rule that the parser is in: what the builder knows of it before its block ends.
struct OpenRule {
    /// The rule, without its declarations and children.
    rule: Rule,
    original_rule_kind: ParsedRuleKind,
    font_feature_maximum_value_count: Option<usize>,
    /// Whether a list of declarations with nothing before it is the declarations of the rule.
    takes_declarations: bool,
    declarations_start: usize,
    declaration_count: usize,
    /// Where the rule's selector list starts in the sheet's arena, for a rule whose prelude is one.
    selectors: Option<RuleSelectors>,
    outer_nesting: NestingTarget,
    outer_parent: NestingTarget,
    /// Where the children of the rule start in the open children.
    first_child: usize,
    /// The declarations in the lists of declarations that are children of an `@property` rule, which its prelude is
    /// made from.
    kept_declarations: Vec<Declaration>,
}

// Parser state is discarded before publishing the owned result.
struct SyntaxParseBuilder<'a> {
    declarations_kind: NestedDeclarationsKind,
    values: Vec<u16>,
    /// The names and value texts of the declarations, which the parse keeps of only the few
    /// declarations it reports: the rest are read while their blocks are built, then dropped.
    declaration_text: Vec<u16>,
    declarations: Vec<ParsedDeclaration>,
    // Declaration ranges are builder state; published lists retain their normalized blocks.
    declaration_ranges: Vec<std::ops::Range<usize>>,
    /// Every rule that the parser ended, each after the rules in it.
    rules: Vec<ParsedRule>,
    /// The indices among `rules` of the rules of the sheet, in the order of the source.
    roots: Vec<u32>,
    /// The children of the rules that the parser ended: those of each rule are together.
    children: Vec<ParsedRuleChild>,
    /// For each rule that the parser ended, the positions among its children of the rules the CSSOM sees.
    native_children: Vec<u32>,
    items: Vec<ParsedRuleChild>,
    identity_count: u64,
    font_feature_values: Vec<u32>,
    diagnostics: Vec<FfiSyntaxDiagnostic>,
    declared_namespaces: Vec<ParserString>,
    // Whether a top-level @import or @namespace rule appended next is still in its ordering window.
    import_rules_valid: bool,
    namespace_rules_valid: bool,
    parse_context: Option<&'a ParseContext<'a>>,
    preserve_property_source_text: bool,
    dedup: DeclarationDedup,
    /// The arena the selectors of the sheet's style rules go into, frozen as the parse finishes.
    selectors: RefCell<crate::css::selector_arena::SheetSelectorsBuilder>,
    /// The rules that the parser is in, from the outermost one.
    open_rules: Vec<OpenRule>,
    /// The children that the open rules have so far, in the order of the source: each open rule says where its own
    /// start. Without an open rule, they are the children of the block that is parsed.
    open_children: Vec<ParsedRuleChild>,
    /// The list of declarations that the parser is in.
    open_declarations: Vec<Declaration>,
    /// Whether the rules that are in no rule are the children of a block, and not the rules of a sheet.
    roots_are_items: bool,
    /// The memory for the component values of the value that is parsed.
    value_scratch: Vec<ComponentValue>,
    /// The memory for the text of the value that is parsed, where the source does not have it as UTF-16.
    text_scratch: Vec<u16>,
    /// The memory for the declarations of the block that is built.
    block_scratch: DeclarationBlockBuilder,
    /// The names that the selectors of the style rules name.
    names: RefCell<crate::css::selector_parser::NameCache>,
    /// What a `&` in the rules being appended names, or none outside every rule.
    nesting: Cell<Option<NestingTarget>>,
    /// What the rule the rules being appended are directly in is to a `&`, or none outside every
    /// rule.
    parent: Cell<Option<NestingTarget>>,
}

/// Lets the declarations and declaration blocks a sheet repeats share one parse, for the length of the sheet's parse.
/// Most of a large sheet's declarations repeat earlier ones: of YouTube's 83k declarations, 11k are distinct, and so
/// are 13k of its 24k blocks. Within one sheet, the same text parses the same way.
#[derive(Default)]
struct DeclarationDedup {
    /// What each declaration text parsed to, by the hash of the text. A text is the name and the value of a declaration
    /// as the source has them, and the declarations with one text have one number, which is its index in the order the
    /// texts came in.
    values: hashbrown::HashTable<DeclarationText>,
    text_count: u32,
    /// For each declaration, by index: twice the number of its text, plus one if it is important. `UNSHARED` for one
    /// that is never shared.
    declaration_keys: Vec<u32>,
    /// The blocks that runs of declarations made, by the hash of the keys of the run. Each is with where the
    /// declarations of the run that made it are.
    blocks: hashbrown::HashTable<(u64, std::ops::Range<usize>, SharedDeclarationBlock)>,
    /// The values the sheet's declarations parsed to, by their content's hash, so that declarations
    /// of different properties, or differently written, that parse to the same value hold one
    /// allocation of it.
    parsed_values: hashbrown::HashTable<(u64, StyleValueArc)>,
    /// The names of the custom properties the sheet declares, so that the declarations of one name
    /// hold one string of it.
    custom_property_names: RefCell<hashbrown::HashTable<CssString>>,
    /// Keyed at random, so that a page cannot make many keys fall into one bucket.
    hasher: foldhash::fast::RandomState,
}

/// A declaration text that the sheet has, and what it parsed to.
struct DeclarationText {
    hash: u64,
    /// Where the first declaration with the text has its name and its value in the source.
    name: std::ops::Range<u32>,
    value: std::ops::Range<u32>,
    /// The number of the text.
    number: u32,
    /// Where the name of the declaration is in the text that the parse keeps.
    name_offset: usize,
    name_length: usize,
    parsed: ParsedDeclarationValue,
}

impl DeclarationDedup {
    const UNSHARED: u32 = u32::MAX;

    /// The allocation of a value equal to `value` the sheet already holds, or `value` itself, which
    /// later equal values then share.
    fn intern_value(&mut self, value: &StyleValueArc) -> StyleValueArc {
        let hash = value.content_hash();
        if let Some((_, interned)) = self.parsed_values.find(hash, |(candidate_hash, candidate)| {
            *candidate_hash == hash && **candidate == **value
        }) {
            return interned.clone();
        }
        self.parsed_values
            .insert_unique(hash, (hash, value.clone()), |(hash, _)| *hash);
        value.clone()
    }

    /// The keys of the `count` declarations from `start`, when all of them may be shared, and the hash of the keys.
    fn block_key(&self, start: usize, count: usize) -> Option<(u64, &[u32])> {
        use std::hash::BuildHasher;
        if count == 0 {
            return None;
        }
        let keys = self.declaration_keys.get(start..start + count)?;
        if keys.contains(&Self::UNSHARED) {
            return None;
        }
        Some((self.hasher.hash_one(keys), keys))
    }
}

type ParsedDeclarationValue = (u16, u8, FfiDeclarationRejection, Option<StyleValueArc>);

// The shared graph owns typed Rust payloads, independently of any FFI views.
pub struct ParsedStyleSheet {
    identity_count: u64,
    /// The text the sheet was parsed from, which DevTools shows: one copy for every document the
    /// parse cache gives this parse.
    source_text: SheetSourceText,
    pub(super) cache_metadata: Option<CachedParseMetadata>,
    declarations_kind: NestedDeclarationsKind,
    values: Box<[u16]>,
    // Raw declarations remain only for root callbacks and diagnostics.
    declarations: Box<[ParsedDeclaration]>,
    root_declarations: Box<[u32]>,
    /// Every rule of the sheet, each after the rules in it.
    rules: Box<[ParsedRule]>,
    /// The arena of the selector lists of the sheet's style rules.
    selectors: crate::css::selector_arena::SelectorArenaHandle,
    /// The indices among `rules` of the top-level rules, in the order of the source.
    roots: Box<[u32]>,
    /// The children of all rules: those of each rule are together.
    children: Box<[ParsedRuleChild]>,
    /// For each rule, the positions among its children of the rules the CSSOM sees.
    native_children: Box<[u32]>,
    items: Box<[ParsedRuleChild]>,
    native_roots: Box<[u32]>,
    /// The positions among `native_roots` of the rules that are, or hold, a rule a media evaluation reads: all of the
    /// sheet an evaluation reaches while the sheet's own media holds as it did.
    media_holding_roots: Box<[u32]>,
    diagnostics: Box<[FfiSyntaxDiagnostic]>,
    declaration_errors: Box<[u32]>,
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
    /// The selector list of `rule`, a style rule of the sheet whose prelude is one.
    fn rule_selectors(&self, rule: &ParsedRule) -> Option<RustParsedSelectorList> {
        Some(RustParsedSelectorList {
            selectors: self.selectors.list(rule.selectors()?.0),
        })
    }

    /// About how many bytes the parse holds beside its text: its values, its rules and their children, and a list of
    /// declarations for each other identity.
    pub(super) fn bytes_kept_past_release(&self) -> usize {
        let declaration_lists = usize::try_from(self.identity_count)
            .expect("a parse fits in memory")
            .saturating_sub(self.rules.len());
        size_of::<Self>()
            + size_of_val(&*self.values)
            + size_of_val(&*self.declarations)
            + size_of_val(&*self.rules)
            + size_of_val(&*self.roots)
            + size_of_val(&*self.children)
            + size_of_val(&*self.native_children)
            + size_of_val(&*self.items)
            + declaration_lists * size_of::<ParsedDeclarationList>()
    }

    /// The rule at `index` among the rules of the sheet.
    fn rule(&self, index: u32) -> &ParsedRule {
        &self.rules[index as usize]
    }

    /// The top-level rule at `position`, in the order of the source.
    fn root(&self, position: usize) -> &ParsedRule {
        self.rule(self.roots[position])
    }

    fn children_of(&self, rule: &ParsedRule) -> &[ParsedRuleChild] {
        &self.children[rule.child_range()]
    }

    /// The positions among the children of `rule` of the rules the CSSOM sees.
    fn native_children_of(&self, rule: &ParsedRule) -> &[u32] {
        &self.native_children[rule.native_child_range()]
    }

    pub(crate) fn identity_count(&self) -> u64 {
        self.identity_count
    }

    pub(crate) fn source_text(&self) -> &SheetSourceText {
        &self.source_text
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
                declarations
                    .source_position
                    .map(|(line, column)| (line as usize, column as usize)),
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
        index: u32,
        declarations_kind: NestedDeclarationsKind,
        list: &NativeRuleList,
    ) -> Option<Rc<NativeRule>> {
        let rule = self.rule(index);
        let kind = rule.native_type()?;
        // NB: A style rule without nested rules makes its empty list only if the CSSOM asks for it.
        let child_rules = kind
            .child_declarations_kind(declarations_kind)
            .filter(|_| kind != NativeRuleType::Style || !self.children_of(rule).is_empty())
            .map(|kind| list.parsed_children(ParsedRuleList::children(self.clone(), index, kind)));
        let position = rule.source_position().filter(|_| kind == NativeRuleType::Style);
        let payload = match kind {
            NativeRuleType::Style => RulePayload::Style(StyleRule::new(
                self.rule_selectors(rule).unwrap(),
                rule.declaration_block().unwrap().clone(),
            )),
            NativeRuleType::Keyframe => {
                RulePayload::Keyframe(Box::new(FfiKeyframe::new(rule.keyframe().unwrap().clone())))
            }
            NativeRuleType::Import => RulePayload::Import {
                data: rule.import_rule().unwrap().clone(),
                media: list
                    .import_media_for(list.identity_for(u64::from(rule.local_identity)))
                    .unwrap_or_else(|| MediaList::new(rule.media_list().unwrap().clone())),
                scope: rule.scope_selectors().cloned(),
            },
            NativeRuleType::Media => RulePayload::Media {
                list: MediaList::new(rule.media_list().unwrap().clone()),
            },
            NativeRuleType::Supports => RulePayload::Supports(rule.supports_condition().unwrap().clone()),
            NativeRuleType::Container => RulePayload::Container(rule.container_conditions().unwrap().clone()),
            NativeRuleType::Scope => RulePayload::Scope(RefCell::new(rule.scope_selectors().unwrap().clone())),
            NativeRuleType::LayerBlock | NativeRuleType::LayerStatement => {
                RulePayload::LayerNames(rule.layer_names().unwrap().clone())
            }
            NativeRuleType::Function => RulePayload::Function(rule.function_signature().unwrap().clone()),
            NativeRuleType::Page => RulePayload::Page {
                selectors: RefCell::new(rule.page_selector_list().unwrap().clone()),
                descriptors: Box::new(FfiDescriptorBlock::new(rule.descriptor_block().unwrap().clone())),
            },
            NativeRuleType::Margin => RulePayload::Margin {
                name: rule.margin_name().unwrap().clone(),
                declarations: Box::new(DeclarationBlock::new(rule.declaration_block().unwrap().clone())),
            },
            NativeRuleType::PositionTry => RulePayload::PositionTry {
                name: rule.position_try_name().unwrap().clone(),
                declarations: Box::new(DeclarationBlock::new(rule.declaration_block().unwrap().clone())),
            },
            NativeRuleType::FontFace => RulePayload::FontFace(Box::new(FfiDescriptorBlock::new(
                rule.descriptor_block().unwrap().clone(),
            ))),
            NativeRuleType::FontFeatureValues => RulePayload::FontFeatureValues(Box::new(FontFeatureValuesRule::new(
                rule.font_feature_values().unwrap().clone(),
            ))),
            NativeRuleType::Keyframes => RulePayload::Keyframes(RefCell::new(rule.keyframes_name().unwrap().clone())),
            NativeRuleType::Namespace => RulePayload::Namespace(rule.namespace_rule().unwrap().clone()),
            NativeRuleType::CounterStyle => {
                RulePayload::CounterStyle(Box::new(FfiCounterStyle::new(rule.counter_style().unwrap().clone())))
            }
            NativeRuleType::Property => RulePayload::Property(rule.property_rule().unwrap().clone()),
            NativeRuleType::NestedDeclarations | NativeRuleType::FunctionDeclarations => unreachable!(),
        };
        Some(NativeRule::with_identity(
            list.identity_for(u64::from(rule.local_identity)),
            kind,
            payload,
            child_rules,
            position,
        ))
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_syntax_native_rules(parse: ArcRef<'_, ParsedStyleSheet>) -> *const NativeRuleList {
    Rc::into_raw(NativeRuleList::from_parsed(parse.to_arc()))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_syntax_root_count(parse: &ParsedStyleSheet) -> usize {
    parse.roots.len() + parse.items.len()
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

/// Whether the parse reported nothing: no diagnostics and no rejected declarations.
#[unsafe(no_mangle)]
pub extern "C" fn rust_css_syntax_parse_is_clean(parse: &ParsedStyleSheet) -> bool {
    parse.diagnostics.is_empty() && parse.declaration_errors.is_empty()
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
            &parse.declarations[index as usize].ffi_view(),
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_syntax_visit_root_declarations(
    parse: &ParsedStyleSheet,
    context: *mut c_void,
    callback: extern "C" fn(*mut c_void, *const u16, usize, &FfiSyntaxDeclaration),
) {
    for &index in &parse.root_declarations {
        callback(
            context,
            parse.values.as_ptr(),
            parse.values.len(),
            &parse.declarations[index as usize].ffi_view(),
        );
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
            parsed_value: self
                .parsed_value
                .as_ref()
                .map_or(std::ptr::null(), StyleValueArc::as_ptr)
                .cast(),
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
        let Some(descriptor) = parse_descriptor(
            context,
            at_rule,
            declaration.name().as_ref(),
            &declaration.value.component_values(),
            declaration.value.text().as_ref(),
        ) else {
            continue;
        };
        let longhands = descriptor_longhands(at_rule, descriptor.id);
        if !longhands.is_empty()
            && let StyleValueData::Shorthand(shorthand_payload) = descriptor.value.as_ref()
        {
            let ShorthandPayload {
                sub_properties, values, ..
            } = &**shorthand_payload;
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
                        name: CssString::from_utf16(&longhand.name.encode_utf16().collect::<Vec<_>>()),
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
                name: CssString::from_utf16(declaration.name().as_ref()),
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
        .component_values()
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
            if rule.prelude.is_empty() {
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
            if matches!(rule.outer_rule_name, Some(OuterRuleName::Context(RuleContext::Style))) && !rule.has_block {
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
        (Rule::At(rule), ParsedRuleKind::PositionTry) => {
            if !rule.has_block {
                Some(FfiSyntaxDiagnosticCode::PositionTryMustBeBlock)
            } else if !matches!(parsed_prelude, ParsedRulePrelude::Name(_)) {
                Some(FfiSyntaxDiagnosticCode::PositionTryInvalidName)
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

fn rule_outer_name(rule: &Rule) -> Option<ParserString> {
    match rule {
        Rule::At(rule) => rule.outer_rule_name.as_ref(),
        Rule::Qualified(rule) => rule.outer_rule_name.as_ref(),
    }
    .and_then(OuterRuleName::name)
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

impl<'a> SyntaxParseBuilder<'a> {
    /// The rule at `index` among the rules that the parser ended.
    fn rule(&self, index: u32) -> &ParsedRule {
        &self.rules[index as usize]
    }

    fn children_of(&self, rule: &ParsedRule) -> &[ParsedRuleChild] {
        &self.children[rule.child_range()]
    }

    fn collect_declaration_errors(&self) -> Box<[usize]> {
        fn visit_item(sheet: &SyntaxParseBuilder<'_>, item: &ParsedRuleChild, errors: &mut Vec<usize>) {
            match item {
                ParsedRuleChild::Declarations(list) => {
                    for index in sheet.declaration_ranges[list.local_identity as usize].clone() {
                        if matches!(
                            sheet.declarations[index].rejection,
                            FfiDeclarationRejection::UnknownProperty | FfiDeclarationRejection::InvalidValue
                        ) {
                            errors.push(index);
                        }
                    }
                }
                &ParsedRuleChild::Rule(rule) => {
                    let rule = sheet.rule(rule);
                    if rule.is_qualified && rule.selectors().is_none() {
                        return;
                    }
                    for child in sheet.children_of(rule) {
                        visit_item(sheet, child, errors);
                    }
                }
            }
        }
        fn visit_rule(sheet: &SyntaxParseBuilder<'_>, rule: &ParsedRule, errors: &mut Vec<usize>) {
            match rule.rule_kind {
                ParsedRuleKind::Qualified if rule.selectors().is_some() => {}
                ParsedRuleKind::Container
                | ParsedRuleKind::Media
                | ParsedRuleKind::Supports
                | ParsedRuleKind::Scope
                | ParsedRuleKind::Layer
                | ParsedRuleKind::Function
                | ParsedRuleKind::Page => {}
                _ => return,
            }
            for child in sheet.children_of(rule) {
                match child {
                    ParsedRuleChild::Declarations(_) => visit_item(sheet, child, errors),
                    &ParsedRuleChild::Rule(rule) => visit_rule(sheet, sheet.rule(rule), errors),
                }
            }
        }
        let mut errors = Vec::new();
        for &root in &self.roots {
            visit_rule(self, self.rule(root), &mut errors);
        }
        for item in &self.items {
            visit_item(self, item, &mut errors);
        }
        errors.into_boxed_slice()
    }

    fn finish(self) -> ParsedStyleSheet {
        // NB: Every handle to the sheet's selectors reads them once the arena is frozen.
        let selectors = self.selectors.borrow().arena();
        drop(self.selectors.take());
        // NB: The arena of the selectors now holds the names, so the parse lets go of them.
        drop(self.names.take());
        let mut declaration_errors = self.collect_declaration_errors().into_vec();
        let mut root_declarations = Vec::new();
        for item in &self.items {
            if let ParsedRuleChild::Declarations(list) = item {
                root_declarations.extend(self.declaration_ranges[list.local_identity as usize].clone());
            }
        }
        let mut new_indices = vec![usize::MAX; self.declarations.len()];
        for &index in declaration_errors.iter().chain(&root_declarations) {
            new_indices[index] = 0;
        }
        let mut declarations = Vec::new();
        let mut values = self.values;
        for (index, mut declaration) in self.declarations.into_iter().enumerate() {
            if new_indices[index] != usize::MAX {
                new_indices[index] = declarations.len();
                // NB: A kept declaration's text moves beside the rest of the text the parse keeps, and empty text too —
                //     or its offset would still point into the text it came from, which can run past the end of this.
                for (offset, length) in [
                    (&mut declaration.name_offset, declaration.name_length),
                    (&mut declaration.value_source_offset, declaration.value_source_length),
                ] {
                    let start = values.len();
                    values.extend_from_slice(&self.declaration_text[*offset..*offset + length]);
                    *offset = start;
                }
                declarations.push(declaration);
            }
        }
        for index in declaration_errors.iter_mut().chain(&mut root_declarations) {
            *index = new_indices[*index];
            assert_ne!(*index, usize::MAX, "published declaration metadata must be retained");
        }
        let narrow = |indices: Vec<usize>| -> Box<[u32]> {
            indices
                .into_iter()
                .map(|index| u32::try_from(index).expect("style sheet has too many declarations"))
                .collect()
        };
        let mut parsed = ParsedStyleSheet {
            selectors,
            identity_count: self.identity_count,
            source_text: SheetSourceText::empty(),
            cache_metadata: None,
            declarations_kind: self.declarations_kind,
            values: values.into_boxed_slice(),
            declarations: declarations.into_boxed_slice(),
            root_declarations: narrow(root_declarations),
            rules: self.rules.into_boxed_slice(),
            roots: self.roots.into_boxed_slice(),
            children: self.children.into_boxed_slice(),
            native_children: self.native_children.into_boxed_slice(),
            items: self.items.into_boxed_slice(),
            native_roots: Box::default(),
            media_holding_roots: Box::default(),
            diagnostics: self.diagnostics.into_boxed_slice(),
            declaration_errors: narrow(declaration_errors),
        };
        parsed.native_roots = if parsed.items.is_empty() {
            parsed
                .roots
                .iter()
                .enumerate()
                .filter_map(|(position, &index)| {
                    parsed
                        .rule(index)
                        .native_type()
                        .map(|_| shared_rules::narrow_index(position))
                })
                .collect()
        } else {
            shared_rules::native_child_indices(&parsed.rules, &parsed.items, None).collect()
        };
        parsed.media_holding_roots = (0..parsed.native_roots.len())
            .filter(|&position| {
                let index = parsed.native_roots[position] as usize;
                if parsed.items.is_empty() {
                    parsed.root(index).holds_media_rules(&parsed)
                } else {
                    let rule = match parsed.items[index] {
                        ParsedRuleChild::Rule(rule) => parsed.rule(rule),
                        ParsedRuleChild::Declarations(_) => return false,
                    };
                    rule.holds_media_rules(&parsed)
                }
            })
            .map(shared_rules::narrow_index)
            .collect();
        parsed
    }

    pub(crate) fn new(parse_context: Option<&'a ParseContext<'a>>, preserve_property_source_text: bool) -> Self {
        let declared_namespaces = parse_context
            .map(declared_namespaces_from_context)
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
            declaration_text: Vec::new(),
            declarations_kind: NestedDeclarationsKind::Style,
            declarations: Vec::new(),
            declaration_ranges: Vec::new(),
            rules: Vec::new(),
            roots: Vec::new(),
            children: Vec::new(),
            native_children: Vec::new(),
            items: Vec::new(),
            identity_count: 0,
            font_feature_values: Vec::new(),
            diagnostics: Vec::new(),
            declared_namespaces,
            import_rules_valid: true,
            namespace_rules_valid: true,
            parse_context,
            preserve_property_source_text,
            dedup: DeclarationDedup::default(),
            selectors: RefCell::new({
                let mut selectors = crate::css::selector_arena::SheetSelectorsBuilder::default();
                if let Some(declared) = parse_context.and_then(|context| context.declared_namespaces) {
                    selectors.declare_namespaces(declared.declarations());
                }
                selectors
            }),
            open_rules: Vec::new(),
            open_children: Vec::new(),
            open_declarations: Vec::new(),
            roots_are_items: false,
            value_scratch: Vec::new(),
            text_scratch: Vec::new(),
            block_scratch: DeclarationBlockBuilder::default(),
            names: RefCell::default(),
            nesting: Cell::new(None),
            parent: Cell::new(None),
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

    fn append_declaration_text(&mut self, text: &[u16]) -> (usize, usize) {
        let offset = self.declaration_text.len();
        self.declaration_text.extend_from_slice(text);
        (offset, text.len())
    }

    fn declaration_name(&self, declaration: &ParsedDeclaration) -> &[u16] {
        &self.declaration_text[declaration.name_offset..declaration.name_offset + declaration.name_length]
    }

    fn append_declaration(
        &mut self,
        declaration: &Declaration,
        font_feature_maximum_value_count: Option<usize>,
    ) -> usize {
        let shareable = declaration.is_property && !declaration.value.may_not_be_shared;
        let hash = shareable.then(|| declaration.value.hash_of_text(&self.dedup.hasher));
        let shared = hash.and_then(|hash| {
            self.dedup.values.find(hash, |text| {
                text.hash == hash && declaration.value.is_same_text_as(&text.name, &text.value)
            })
        });
        let (text_number, name_offset, name_length, (property_id, descriptor_id, rejection, parsed_value)) =
            match shared {
                // NB: Declarations with the same text have the same name, and share the kept text of it.
                Some(text) => (text.number, text.name_offset, text.name_length, text.parsed.clone()),
                None => {
                    let (name_offset, name_length) = self.append_declaration_text(declaration.name().as_ref());
                    let mut value = self.parse_declaration_value(declaration);
                    // NB: A value taking part in a substitution keeps its own: it is resolved for the
                    //     property declaring it, and the longhands a shorthand awaiting substitution
                    //     expands to name it by identity.
                    if shareable
                        && let Some(parsed) = &mut value.3
                        && !matches!(
                            **parsed,
                            StyleValueData::Unresolved(_)
                                | StyleValueData::PendingSubstitution { .. }
                                | StyleValueData::Shorthand(_)
                        )
                    {
                        *parsed = self.dedup.intern_value(parsed);
                    }
                    let number = self.dedup.text_count;
                    if let Some(hash) = hash {
                        self.dedup.text_count += 1;
                        let (name, range) = declaration.value.ranges();
                        self.dedup.values.insert_unique(
                            hash,
                            DeclarationText {
                                hash,
                                name,
                                value: range,
                                number,
                                name_offset,
                                name_length,
                                parsed: value.clone(),
                            },
                            |text| text.hash,
                        );
                    }
                    (number, name_offset, name_length, value)
                }
            };
        let declaration_key = match hash {
            Some(_) => text_number * 2 + u32::from(declaration.important),
            None => DeclarationDedup::UNSHARED,
        };
        let declaration_index = self.declarations.len();
        if self.dedup.declaration_keys.len() < declaration_index {
            self.dedup
                .declaration_keys
                .resize(declaration_index, DeclarationDedup::UNSHARED);
        }
        self.dedup.declaration_keys.push(declaration_key);
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
            self.append_declaration_text(&declaration.value.text())
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
            start_line: declaration.source_position.line as usize,
            start_column: declaration.source_position.column as usize,
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
    ) -> (u16, u8, FfiDeclarationRejection, Option<StyleValueArc>) {
        let Some(parse_context) = self.parse_context else {
            return (u16::MAX, u8::MAX, FfiDeclarationRejection::None, None);
        };
        if !declaration.is_property {
            let Some(at_rule) = descriptor_at_rule(declaration.rule_context) else {
                return (u16::MAX, u8::MAX, FfiDeclarationRejection::None, None);
            };
            return match parse_descriptor(
                parse_context,
                at_rule,
                declaration.name().as_ref(),
                &declaration.value.component_values(),
                &declaration.value.text(),
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
        let name = declaration.name();
        let Some(property_id) = property_id_from_name(name.as_slice()) else {
            // NB: Mirrors the ignored-vendor and unknown-property distinction from commit
            //     dbe9950abd9 in Libraries/LibWeb/CSS/Parser/Parser.cpp:360.
            let rejection = if has_ignored_vendor_prefix(name.as_slice().into()) {
                FfiDeclarationRejection::IgnoredVendorPrefix
            } else {
                FfiDeclarationRejection::UnknownProperty
            };
            return (u16::MAX, u8::MAX, rejection, None);
        };

        let mut value_contexts =
            smallvec::SmallVec::<[FfiValueParsingContext; 8]>::from_slice(parse_context.value_contexts.as_slice());
        value_contexts.push(FfiValueParsingContext {
            kind: FfiValueParsingContextKind::Property,
            value: property_id,
            secondary_value: 0,
            name: Default::default(),
        });
        let mut context = *parse_context;
        context.value_contexts = ak::Span::new(&value_contexts);

        // NB: Each value uses the memory of the one before it for its component values and its text.
        let mut values = std::mem::take(&mut self.value_scratch);
        declaration.value.append_component_values_to(&mut values);
        let text = declaration.value.text_in(&mut self.text_scratch);
        let outcome = parse_css_value_with_utf16_source(&context, property_id, &values, text);
        values.clear();
        self.value_scratch = values;
        if let Some(random_function_index) = parse_context.random_function_index {
            random_function_index.set(0);
        }
        match outcome {
            ParseOutcome::Parsed(value) => (property_id, u8::MAX, FfiDeclarationRejection::None, Some(value)),
            ParseOutcome::Invalid | ParseOutcome::NotHandled => {
                (property_id, u8::MAX, FfiDeclarationRejection::InvalidValue, None)
            }
        }
    }

    fn append_collected_declaration(&mut self, descriptor: &DescriptorData) {
        let (name_offset, name_length) = self.append_declaration_text(descriptor.name.units());
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
        if let Some(context) = self.parse_context
            && declarations
                .first()
                .is_some_and(|declaration| descriptor_at_rule(declaration.rule_context).is_some())
        {
            let descriptors = collect_descriptors(declarations, context);
            for descriptor in &descriptors {
                self.append_collected_declaration(descriptor);
            }
            return (
                start,
                self.declarations.len() - start,
                Some(Arc::new(DescriptorBlockData {
                    descriptors: descriptors.into_iter().collect(),
                })),
            );
        }
        for declaration in declarations {
            self.append_declaration(declaration, font_feature_maximum_value_count);
        }
        (start, declarations.len(), None)
    }

    fn append_declaration_block(
        &self,
        block: &mut DeclarationBlockBuilder,
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
                let name = self.declaration_name(declaration);
                let hash = |units: &[u16]| std::hash::BuildHasher::hash_one(&self.dedup.hasher, units);
                let name = self
                    .dedup
                    .custom_property_names
                    .borrow_mut()
                    .entry(
                        hash(name),
                        |string| string.units() == name,
                        |string| hash(string.units()),
                    )
                    .or_insert_with(|| CssString::from_utf16(name))
                    .get()
                    .clone();
                block.append_custom_property(name, property);
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

    /// Appends a list of declarations that is a child of a rule, or of the block that is parsed.
    fn append_declaration_list(
        &mut self,
        declarations: &[Declaration],
        font_feature_maximum_value_count: Option<usize>,
    ) -> ParsedRuleChild {
        let (start, count, descriptor_block) = self.append_declarations(declarations, font_feature_maximum_value_count);
        let mut block = std::mem::take(&mut self.block_scratch);
        self.append_declaration_block(
            &mut block,
            start,
            count,
            // https://drafts.csswg.org/css-anchor-position-1/#fallback-rule
            // It is invalid to use !important on the properties in the <declaration-list>. Doing so causes the
            // property it is used on to become invalid, but does not invalidate the @position-try rule as a whole.
            declarations.first().is_some_and(|declaration| {
                matches!(
                    declaration.rule_context,
                    RuleContext::Keyframe | RuleContext::AtPositionTry
                )
            }),
        );
        let declaration_block = Some(block.finish_and_reuse());
        self.block_scratch = block;
        let local_identity = self.next_identity();
        let identity_index = usize::try_from(local_identity).expect("parsed declaration identity overflow");
        self.declaration_ranges
            .resize(identity_index.checked_add(1).unwrap(), 0..0);
        self.declaration_ranges[identity_index] = start..start + count;
        let source_position = (count != 0).then(|| {
            let first = &self.declarations[start];
            let narrow = |value: usize| u32::try_from(value).expect("style sheet text exceeds u32");
            (narrow(first.start_line), narrow(first.start_column))
        });
        ParsedRuleChild::Declarations(Box::new(ParsedDeclarationList {
            local_identity,
            source_position,
            declaration_block,
            descriptor_block,
        }))
    }

    fn parse_selector_list(
        &self,
        prelude: &structure::Prelude,
        prelude_rows: Option<&Rows>,
        selector_type: SelectorType,
        nesting_parent: StyleNestingParent,
        nesting: NestingTarget,
    ) -> Option<(RustParsedSelectorList, Option<RuleOrdinal>)> {
        let declared_namespaces = self
            .declared_namespaces
            .iter()
            .map(|namespace| TokenizerInput::Utf16(namespace.as_ref()))
            .collect::<Vec<_>>();
        // NB: Tokens that are nested too deep are not a list of component values, so they are not selectors.
        let selector_list = parse_selector_list_of_rows(
            prelude_rows?,
            &self.names,
            prelude.input(),
            &declared_namespaces,
            selector_type,
        )
        .ok()?;
        let (list, ordinal) = selector_list
            .adapt_for_nesting(nesting_parent)
            .into_rule_list_in(&mut self.selectors.borrow_mut(), nesting);
        Some((list, ordinal))
    }

    fn rule_descriptors(children: &[ParsedRuleChild], kind: ParsedRuleKind) -> Option<Arc<DescriptorBlockData>> {
        // NB: A single list's block is shared as is; the descriptors are merged only once a second list appends.
        let mut first: Option<&Arc<DescriptorBlockData>> = None;
        let mut merged: Option<Vec<DescriptorData>> = None;
        for child in children {
            let ParsedRuleChild::Declarations(child) = child else {
                continue;
            };
            let Some(block) = &child.descriptor_block else { continue };
            let merged = match (&mut merged, first) {
                (Some(merged), _) => merged,
                (None, Some(first)) => merged.insert(first.descriptors.to_vec()),
                (None, None) => {
                    first = Some(block);
                    continue;
                }
            };
            for descriptor in &block.descriptors {
                append_collected_descriptor(merged, descriptor.clone());
            }
        }
        let data = match merged {
            Some(merged) => Some(Arc::new(DescriptorBlockData {
                descriptors: merged.into_iter().collect(),
            })),
            None => first.cloned(),
        };
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

    /// Starts a rule: parses its selectors, and says what a `&` names in the rules in it.
    fn begin_rule(&mut self, rule: Rule, prelude_rows: Option<&Rows>) {
        let original_rule_kind = rule_kind(&rule);
        let font_feature_maximum_value_count = match &rule {
            Rule::At(rule) => font_feature_values_rule(rule.name.as_ref()).map(|(_, maximum)| maximum),
            Rule::Qualified(_) => None,
        };
        // NB: A rule's selectors go into the sheet's arena before those of the rules nested in it,
        //     whose `&` names them by their ordinal there.
        let outer_nesting = self.nesting.get().unwrap_or(match rule_nesting_parent(&rule) {
            StyleNestingParent::None => NestingTarget::Root,
            StyleNestingParent::Scope => NestingTarget::Scope,
            StyleNestingParent::Style => NestingTarget::Unbound,
        });
        let (selectors, rule_ordinal) = match &rule {
            Rule::Qualified(rule) if rule.prelude_is_selector => self
                .parse_selector_list(
                    &rule.prelude,
                    prelude_rows,
                    if rule.nesting_parent != StyleNestingParent::None {
                        SelectorType::Relative
                    } else {
                        SelectorType::Standalone
                    },
                    rule.nesting_parent,
                    outer_nesting,
                )
                .map_or((None, None), |(list, ordinal)| {
                    (
                        Some(RuleSelectors(self.selectors.borrow().start_of(&list.selectors))),
                        ordinal,
                    )
                }),
            _ => (None, None),
        };
        // NB: A `&` in a `<scope-start>` names the rule the `@scope` is directly in.
        let outer_parent = self.parent.get().unwrap_or(outer_nesting);
        let (inner_nesting, inner_parent) = match &rule {
            Rule::Qualified(rule) if rule.prelude_is_selector => {
                let nesting = rule_ordinal.map_or(NestingTarget::Unbound, NestingTarget::Rule);
                (nesting, nesting)
            }
            Rule::At(_) if original_rule_kind == ParsedRuleKind::Scope => (NestingTarget::Scope, NestingTarget::Scope),
            _ => (outer_nesting, NestingTarget::Root),
        };
        self.nesting.set(Some(inner_nesting));
        self.parent.set(Some(inner_parent));
        self.open_rules.push(OpenRule {
            takes_declarations: matches!(rule, Rule::Qualified(_)),
            rule,
            original_rule_kind,
            font_feature_maximum_value_count,
            declarations_start: self.declarations.len(),
            declaration_count: 0,
            selectors,
            outer_nesting,
            outer_parent,
            first_child: self.open_children.len(),
            kept_declarations: Vec::new(),
        });
    }

    /// Ends the list of declarations that is open, if one is. The first list of a qualified rule, with nothing before
    /// it, is the declarations of the rule. Each other list is a child of the rule it is in.
    fn end_declarations(&mut self) {
        if self.open_declarations.is_empty() {
            return;
        }
        let declarations = std::mem::take(&mut self.open_declarations);
        let child_count = self.open_children.len();
        match self.open_rules.last_mut() {
            Some(open) if open.takes_declarations && open.first_child == child_count => {
                open.takes_declarations = false;
                let (start, count, _) = self.append_declarations(&declarations, None);
                let open = self.open_rules.last_mut().unwrap();
                open.declarations_start = start;
                open.declaration_count = count;
            }
            mut open => {
                if let Some(open) = &mut open
                    && open.original_rule_kind == ParsedRuleKind::Property
                {
                    open.kept_declarations.extend(declarations.iter().cloned());
                }
                let font_feature_maximum_value_count = open.and_then(|open| open.font_feature_maximum_value_count);
                let child = self.append_declaration_list(&declarations, font_feature_maximum_value_count);
                self.open_children.push(child);
            }
        }
        // NB: The next list uses the memory of this one.
        self.open_declarations = declarations;
        self.open_declarations.clear();
    }

    /// Ends the rule that is open, and makes it a child of the rule it is in, or a rule of the sheet.
    fn end_rule(&mut self) {
        let open = self.open_rules.pop().expect("a rule is open");
        // NB: The children of the rule move to the end of the children of the sheet, where they stay together.
        let first_child = self.children.len();
        self.children.extend(self.open_children.drain(open.first_child..));
        self.nesting.set(Some(open.outer_nesting));
        self.parent.set(Some(open.outer_parent));
        let parsed = self.finish_rule(&open, first_child..self.children.len());
        let index = shared_rules::narrow_index(self.rules.len());
        self.rules.push(parsed);
        if self.open_rules.is_empty() && !self.roots_are_items {
            self.append_root(&open.rule, index);
        } else {
            self.open_children.push(ParsedRuleChild::Rule(index));
        }
    }

    /// Makes the rule `open` is, whose children are at `children` among the children of the sheet.
    fn finish_rule(&mut self, open: &OpenRule, children: std::ops::Range<usize>) -> ParsedRule {
        let rule = &open.rule;
        let original_rule_kind = open.original_rule_kind;
        let (is_qualified, name, has_block, source_position) = match rule {
            Rule::At(rule) => (false, rule.name.as_ref(), rule.has_block, None),
            Rule::Qualified(rule) => (true, &[][..], true, rule.source_position),
        };
        let (declarations_start, declaration_count) = (open.declarations_start, open.declaration_count);
        let selectors = open.selectors;
        let outer_parent = open.outer_parent;
        let descriptor_block = Self::rule_descriptors(&self.children[children.clone()], original_rule_kind);
        let parse_context = self.parse_context;
        let declared_namespaces = self
            .declared_namespaces
            .iter()
            .map(|namespace| TokenizerInput::Utf16(namespace.as_ref()))
            .collect::<Vec<_>>();
        let parsed_prelude = parse_rule_prelude(
            rule,
            &open.kept_declarations,
            original_rule_kind,
            parse_context,
            &declared_namespaces,
            &resolve_query_feature,
        );
        let validation_code =
            structural_validation_code(rule, original_rule_kind, &parsed_prelude, selectors.is_some());
        let rule_kind = if let Some(code) = validation_code {
            let prelude_source = match rule {
                Rule::At(rule) => component_list_source(&rule.prelude),
                Rule::Qualified(rule) => Cow::Owned(rule.prelude.text()),
            };
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
                for child in &self.children[children.clone()] {
                    // FIXME: Handle the font-display descriptor here, see
                    //        https://drafts.csswg.org/css-fonts-4/#font-display-font-feature-values
                    let &ParsedRuleChild::Rule(child) = child else {
                        continue;
                    };
                    let child = self.rule(child);
                    let ParsedRulePayload::FontFeatureValuesRule(kind) = child.payload else {
                        continue;
                    };
                    for item in self.children_of(child) {
                        let ParsedRuleChild::Declarations(item) = item else {
                            continue;
                        };
                        for declaration in
                            &self.declarations[self.declaration_ranges[item.local_identity as usize].clone()]
                        {
                            if declaration.font_feature_value_count == 0 {
                                continue;
                            }
                            let name = self.declaration_name(declaration);
                            let values = &self.font_feature_values[declaration.font_feature_values_start
                                ..declaration.font_feature_values_start + declaration.font_feature_value_count];
                            data.set(kind, name, values.to_vec());
                        }
                    }
                }
                font_feature_values = Some(Arc::new(data));
            }
            ParsedRulePrelude::Scope { start, end } => {
                // NB: The prelude goes into the sheet's arena, where a `&` in its start names what
                //     encloses the `@scope`.
                let mut selectors = self.selectors.borrow_mut();
                let mut repack = |list: RustParsedSelectorList, nesting| RustParsedSelectorList {
                    selectors: selectors.push_copy(&list.selectors, nesting),
                };
                scope_selectors = Some(Arc::new(ScopeSelectors {
                    start: start.map(|start| repack(start, outer_parent)),
                    end: end.map(|end| repack(end, NestingTarget::Root)),
                }));
            }
            ParsedRulePrelude::Import(import) => {
                if import.has_scope {
                    scope_selectors = Some(Arc::new(ScopeSelectors {
                        start: import.scope_start,
                        end: import.scope_end,
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
                media_list = Some(Arc::new(MediaListData::from_queries(import.media_queries)));
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
                            name: ak::Utf16FlyString::from_utf16(&parameter.name),
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
                media_list = Some(Arc::new(MediaListData::from_queries(queries)));
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
            // NB: A style rule's block is its own declarations, so the same text gives the same block.
            let block_key = qualified_rule
                .prelude_is_selector
                .then(|| self.dedup.block_key(declarations_start, declaration_count))
                .flatten();
            let block_hash = block_key.map(|(hash, _)| hash);
            if let Some(shared) = block_key.and_then(|(hash, keys)| {
                self.dedup
                    .blocks
                    .find(hash, |(block_hash, run, _)| {
                        *block_hash == hash && self.dedup.declaration_keys[run.clone()] == *keys
                    })
                    .map(|(_, _, block)| block)
            }) {
                Some(shared.clone())
            } else {
                let mut block = std::mem::take(&mut self.block_scratch);
                self.append_declaration_block(
                    &mut block,
                    declarations_start,
                    declaration_count,
                    !qualified_rule.prelude_is_selector,
                );
                if !qualified_rule.prelude_is_selector {
                    for item in &self.children[children.clone()] {
                        if let ParsedRuleChild::Declarations(item) = item {
                            let range = &self.declaration_ranges[item.local_identity as usize];
                            self.append_declaration_block(&mut block, range.start, range.len(), true);
                        }
                    }
                }
                let finished = block.finish_and_reuse();
                self.block_scratch = block;
                let block = finished;
                if let Some(hash) = block_hash {
                    self.dedup.blocks.insert_unique(
                        hash,
                        (
                            hash,
                            declarations_start..declarations_start + declaration_count,
                            block.clone(),
                        ),
                        |(hash, ..)| *hash,
                    );
                }
                Some(block)
            }
        } else if matches!(rule_kind, ParsedRuleKind::Margin | ParsedRuleKind::PositionTry) {
            Some(declaration_block_from_blocks(
                self.children[children.clone()].iter().filter_map(|item| match item {
                    ParsedRuleChild::Declarations(item) => item.declaration_block.as_ref(),
                    ParsedRuleChild::Rule(_) => None,
                }),
            ))
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
        let position_try_name = (rule_kind == ParsedRuleKind::PositionTry)
            .then(|| CssString::from_utf16(parsed_prelude_name.as_deref().unwrap()));
        let counter_style = if rule_kind == ParsedRuleKind::CounterStyle {
            Some(Arc::new(CounterStyleData {
                name: CssString::from_utf16(parsed_prelude_name.as_deref().unwrap()),
                descriptors: descriptor_block.as_ref().unwrap().clone(),
            }))
        } else {
            None
        };
        let first_native_child = self.native_children.len();
        if keyframe.is_none() {
            self.native_children.extend(shared_rules::native_child_indices(
                &self.rules,
                &self.children[children.clone()],
                Some(rule_kind),
            ));
        }
        let native_child_count = self.native_children.len() - first_native_child;
        // NB: A rule's prelude, or its kind, gives it one payload at most.
        let payload = if let Some(keyframe) = keyframe {
            ParsedRulePayload::Keyframe(keyframe)
        } else if is_qualified {
            ParsedRulePayload::Style {
                selectors,
                declaration_block: declaration_block.unwrap(),
            }
        } else if let Some(import_rule) = import_rule {
            ParsedRulePayload::Import(Box::new(ImportPayload {
                import_rule,
                media_list: media_list.unwrap(),
            }))
        } else if let Some(media_list) = media_list {
            ParsedRulePayload::Media(media_list)
        } else if let Some(condition) = supports_condition {
            ParsedRulePayload::Supports(condition)
        } else if let Some(conditions) = container_conditions {
            ParsedRulePayload::Container(conditions)
        } else if let Some(selectors) = scope_selectors {
            ParsedRulePayload::Scope(selectors)
        } else if let Some(names) = layer_names {
            ParsedRulePayload::Layer(names)
        } else if let Some(selectors) = page_selector_list {
            ParsedRulePayload::Page(Box::new(PagePayload {
                selectors,
                descriptors: descriptor_block.expect("a page rule has descriptors"),
            }))
        } else if rule_kind == ParsedRuleKind::FontFace {
            ParsedRulePayload::FontFace(descriptor_block.expect("a font-face rule has descriptors"))
        } else if rule_kind == ParsedRuleKind::FontFeatureValuesRule {
            ParsedRulePayload::FontFeatureValuesRule(font_feature_values_rule(name).unwrap().0)
        } else if let Some(values) = font_feature_values {
            ParsedRulePayload::FontFeatureValues(values)
        } else if let Some(name) = keyframes_name {
            ParsedRulePayload::Keyframes(name)
        } else if let Some(name) = position_try_name {
            ParsedRulePayload::PositionTry(Box::new(NamedBlockPayload {
                name,
                declaration_block: declaration_block.unwrap(),
            }))
        } else if let Some(declaration_block) = declaration_block {
            ParsedRulePayload::Margin(Box::new(NamedBlockPayload {
                name: CssString::from_utf16(
                    &name
                        .iter()
                        .map(|&unit| crate::css::ffi_support::ascii_lowercase(unit))
                        .collect::<Vec<_>>(),
                ),
                declaration_block,
            }))
        } else if let Some(counter_style) = counter_style {
            ParsedRulePayload::CounterStyle(counter_style)
        } else if let Some(namespace) = namespace_rule {
            ParsedRulePayload::Namespace(namespace)
        } else if let Some(property) = property_rule {
            ParsedRulePayload::Property(property)
        } else if let Some(signature) = function_signature {
            ParsedRulePayload::Function(signature)
        } else {
            ParsedRulePayload::None
        };
        ParsedRule {
            local_identity: u32::try_from(self.next_identity()).expect("style sheet has too many rules"),
            is_qualified,
            rule_kind,
            has_block,
            has_source_position: source_position.is_some(),
            start_line: source_position.map_or(0, |position| position.line),
            start_column: source_position.map_or(0, |position| position.column),
            first_child: shared_rules::narrow_index(children.start),
            child_count: shared_rules::narrow_index(children.len()),
            first_native_child: shared_rules::narrow_index(first_native_child),
            native_child_count: shared_rules::narrow_index(native_child_count),
            payload,
        }
    }

    /// Makes the rule at `index` among the rules a rule of the sheet.
    fn append_root(&mut self, rule: &Rule, index: u32) {
        // NB: Invalid rules do not close either ordering window, and statement @layer rules do not
        //     close the import or namespace window.
        let parsed = &self.rules[index as usize];
        let rule_kind = parsed.rule_kind;
        match rule_kind {
            ParsedRuleKind::Invalid | ParsedRuleKind::Unknown | ParsedRuleKind::IgnoredVendor => {}
            ParsedRuleKind::Layer if !parsed.has_block => {}
            ParsedRuleKind::Import if self.import_rules_valid => {}
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
                self.rules[index as usize].rule_kind = ParsedRuleKind::Invalid;
            }
            ParsedRuleKind::Namespace if self.namespace_rules_valid => {
                self.import_rules_valid = false;
                let namespace = self.rules[index as usize].namespace_rule().unwrap();
                self.selectors
                    .borrow_mut()
                    .declare_namespace(namespace.prefix.units(), namespace.uri.units());
                let prefix = namespace.prefix.units();
                if !self
                    .declared_namespaces
                    .iter()
                    .any(|namespace| namespace.as_ref() == prefix)
                {
                    self.declared_namespaces.push(ParserString::from(Box::from(prefix)));
                }
            }
            ParsedRuleKind::Namespace => {
                self.import_rules_valid = false;
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
                self.rules[index as usize].rule_kind = ParsedRuleKind::Invalid;
            }
            _ => {
                self.import_rules_valid = false;
                self.namespace_rules_valid = false;
            }
        }
        self.roots.push(index);
    }
}

impl structure::Sink for SyntaxParseBuilder<'_> {
    fn begin_rule(&mut self, rule: Rule, prelude: Option<&Rows>) {
        SyntaxParseBuilder::begin_rule(self, rule, prelude);
    }

    fn end_rule(&mut self) {
        SyntaxParseBuilder::end_rule(self);
    }

    fn declaration(&mut self, declaration: Declaration) {
        self.open_declarations.push(declaration);
    }

    fn end_declarations(&mut self) {
        SyntaxParseBuilder::end_declarations(self);
    }
}

pub(crate) fn parse_shared_stylesheet(
    source: TokenizerInput<'_>,
    parse_context: Option<&ParseContext<'_>>,
) -> Arc<ParsedStyleSheet> {
    parse_with_cache(source, parse_context, || {
        parse_stylesheet_uncached(source, parse_context)
    })
}

/// Parses a style sheet without asking the process's parse cache, or offering it the result.
pub(crate) fn parse_stylesheet_uncached(
    source: TokenizerInput<'_>,
    parse_context: Option<&ParseContext<'_>>,
) -> ParsedStyleSheet {
    let text = source_of(source);
    let mut parser = Parser::new(&text, Vec::new());
    let mut parse = SyntaxParseBuilder::new(parse_context, false);
    parser.parse_stylesheet(&mut parse);
    // The tokens' own diagnostics come ahead of the rules'.
    parse.diagnostics.splice(0..0, parser.token_diagnostics());
    let mut parsed = parse.finish();
    parsed.source_text = SheetSourceText::new(source);
    parsed
}

/// Every rule context, at the index of its discriminant.
const RULE_CONTEXTS: [RuleContext; RuleContext::AtPositionTry as usize + 1] = {
    use RuleContext::*;
    let contexts = [
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
        AtPositionTry,
    ];
    let mut index = 0;
    while index < contexts.len() {
        assert!(contexts[index] as usize == index);
        index += 1;
    }
    contexts
};

fn rule_contexts(contexts: &[u8]) -> Option<Vec<RuleContext>> {
    contexts
        .iter()
        .map(|&context| RULE_CONTEXTS.get(usize::from(context)).copied())
        .collect()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_css_stylesheet_syntax(
    source: FfiUtf16View,
    parse_context: Option<&ParseContext<'_>>,
) -> *const ParsedStyleSheet {
    let Some(source) = source.units() else {
        return std::ptr::null();
    };
    Arc::into_raw(parse_shared_stylesheet(source, parse_context))
}

/// Parses exactly one CSS rule into a Rust-owned arena.
#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_css_rule_syntax(
    source: FfiUtf16View,
    contexts: ak::Span<'_, u8>,
    nested: bool,
    parse_context: Option<&ParseContext<'_>>,
) -> *const ParsedStyleSheet {
    let Some(source) = source.units() else {
        return std::ptr::null_mut();
    };
    let Some(contexts) = rule_contexts(contexts.as_slice()) else {
        return std::ptr::null_mut();
    };
    Arc::into_raw(parse_shared_rule(source, contexts, nested, parse_context))
}

pub(crate) fn parse_shared_rule(
    source: TokenizerInput<'_>,
    contexts: Vec<RuleContext>,
    nested: bool,
    parse_context: Option<&ParseContext<'_>>,
) -> Arc<ParsedStyleSheet> {
    let source = source_of(source);
    let mut parser = Parser::new(&source, contexts);
    let mut parse = SyntaxParseBuilder::new(parse_context, false);
    if parser.rule_context().contains(&RuleContext::AtFunction) {
        parse.declarations_kind = NestedDeclarationsKind::Function;
    }
    if !parser.parse_only_rule(nested, &mut parse) {
        // NB: The source is not one rule, so nothing of what the parser read is the result.
        let declarations_kind = parse.declarations_kind;
        parse = SyntaxParseBuilder::new(parse_context, false);
        parse.declarations_kind = declarations_kind;
    }
    parse.diagnostics.splice(0..0, parser.token_diagnostics());
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
#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_page_selector_list(source: FfiUtf16View) -> *const PageSelectorList {
    let Some(source) = source.units() else {
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

#[unsafe(no_mangle)]
pub extern "C" fn rust_page_selector_list_release(list: OwnedArc<PageSelectorList>) {
    drop(list);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_page_selector_list_serialize(
    list: &PageSelectorList,
    context: *mut c_void,
    visit: crate::css::parser::query_parser::VisitQuerySerialization,
) {
    let text = serialized_page_selectors(list);
    let units = text.to_utf16();
    unsafe { visit(context, units.as_ptr(), units.len()) };
}

/// The text of a list of page selectors.
pub(crate) fn serialized_page_selectors(list: &PageSelectorList) -> ak::Utf16String {
    let mut text = TextSink::new();
    for (index, selector) in list.selectors.iter().enumerate() {
        if index != 0 {
            text.push_ascii(", ");
        }
        if let Some(name) = &selector.name {
            name.iter().for_each(|&unit| text.push_code_unit(unit));
        }
        for pseudo_class in &selector.pseudo_classes {
            let name = match pseudo_class {
                PagePseudoClass::Left => ":left",
                PagePseudoClass::Right => ":right",
                PagePseudoClass::First => ":first",
                PagePseudoClass::Blank => ":blank",
            };
            text.push_ascii(name);
        }
    }
    text.into_utf16_string()
}

/// Retains a normalized declaration block from parsed block contents, ignoring rules.
#[unsafe(no_mangle)]
pub extern "C" fn rust_css_syntax_parse_declaration_block(
    parse: &ParsedStyleSheet,
) -> *mut crate::css::declaration_block::DeclarationBlock {
    use crate::css::declaration_block::DeclarationBlock;
    let parsed = parse;
    let declarations = declaration_block_from_blocks(parsed.items.iter().filter_map(|item| match item {
        ParsedRuleChild::Declarations(list) => list.declaration_block.as_ref(),
        ParsedRuleChild::Rule(_) => None,
    }));
    Box::into_raw(Box::new(DeclarationBlock::new(declarations)))
}

/// Retains parsed descriptor lists from block contents, ignoring rules.
#[unsafe(no_mangle)]
pub extern "C" fn rust_css_syntax_parse_descriptor_block(
    parsed: &ParsedStyleSheet,
) -> *mut crate::css::descriptor_block::FfiDescriptorBlock {
    use crate::css::descriptor_block::FfiDescriptorBlock;
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
            let data = Arc::make_mut(descriptors);
            data.descriptors = data.descriptors.iter().chain(&block.descriptors).cloned().collect();
        } else {
            descriptors = Some(block.clone());
        }
    }
    // 4. Return parsed declarations.
    Box::into_raw(Box::new(FfiDescriptorBlock::new(descriptors.unwrap_or_default())))
}

/// Parses block contents into a Rust-owned arena.
#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_css_block_syntax(
    source: FfiUtf16View,
    contexts: ak::Span<'_, u8>,
    parse_context: Option<&ParseContext<'_>>,
    preserve_property_source_text: bool,
) -> *const ParsedStyleSheet {
    let Some(source) = source.units() else {
        return std::ptr::null_mut();
    };
    let Some(contexts) = rule_contexts(contexts.as_slice()) else {
        return std::ptr::null_mut();
    };
    Arc::into_raw(parse_shared_block(
        source,
        contexts,
        parse_context,
        preserve_property_source_text,
    ))
}

pub(crate) fn parse_shared_block(
    source: TokenizerInput<'_>,
    contexts: Vec<RuleContext>,
    parse_context: Option<&ParseContext<'_>>,
    preserve_property_source_text: bool,
) -> Arc<ParsedStyleSheet> {
    let source = source_of(source);
    let mut parser = Parser::new(&source, contexts);
    let mut parse = SyntaxParseBuilder::new(parse_context, preserve_property_source_text);
    if parser.rule_context().contains(&RuleContext::AtFunction) {
        parse.declarations_kind = NestedDeclarationsKind::Function;
    }
    parse.roots_are_items = true;
    parser.parse_block_contents(&mut parse);
    parse.items = std::mem::take(&mut parse.open_children);
    parse.diagnostics.splice(0..0, parser.token_diagnostics());
    Arc::new(parse.finish())
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_syntax_parse_free(parse: OwnedArc<ParsedStyleSheet>) {
    drop(parse);
}

#[cfg(test)]
mod tests {
    use super::{
        FfiSyntaxDiagnosticCode, PagePseudoClass, ParsedRuleChild, ParsedRuleKind, ParsedRulePrelude, ParsedStyleSheet,
        RuleContext, consume_a_list_of_component_values, parse_keyframe_selectors, parse_page_selector_list,
        tokenize_for_parser,
    };
    use crate::css::parser::value_parser::ParseContext;

    fn utf16(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }

    pub(super) fn parse_context() -> ParseContext<'static> {
        ParseContext::default()
    }

    fn parse_test_stylesheet(source: &[u8]) -> ParsedStyleSheet {
        super::parse_stylesheet_uncached(source.into(), Some(&parse_context()))
    }

    #[test]
    fn a_sheet_indexes_the_top_level_rules_that_hold_media_rules() {
        let parsed = parse_test_stylesheet(
            b"a { color: red } @media (min-width: 1px) { b { color: blue } } \
              c { d { color: green } @media print { color: red } } @supports (color: red) { e { color: red } } \
              f { color: red }",
        );
        assert_eq!(&*parsed.media_holding_roots, &[1, 2]);
    }

    #[test]
    fn a_sheet_already_being_parsed_is_parsed_once() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        use std::sync::{Arc, Barrier};
        let source = Arc::new(utf16("a { color: red } b { color: blue }"));
        let parses = Arc::new(AtomicUsize::new(0));
        // The first worker's parse starts, then holds until the second worker has asked for the same sheet.
        let parse_started = Arc::new(Barrier::new(2));
        let parse_may_finish = Arc::new(Barrier::new(2));
        let parse = |held: Option<(Arc<Barrier>, Arc<Barrier>)>, source: Arc<Vec<u16>>, parses: Arc<AtomicUsize>| {
            move || {
                let context = parse_context();
                let input = crate::css::css_tokenizer::TokenizerInput::Utf16(&source);
                super::super::stylesheet_cache::parse_with_cache(input, Some(&context), || {
                    parses.fetch_add(1, Ordering::SeqCst);
                    if let Some((started, may_finish)) = &held {
                        started.wait();
                        may_finish.wait();
                    }
                    parse_test_stylesheet(b"a { color: red } b { color: blue }")
                })
            }
        };
        let first = std::thread::spawn(parse(
            Some((parse_started.clone(), parse_may_finish.clone())),
            source.clone(),
            parses.clone(),
        ));
        parse_started.wait();
        let second = std::thread::spawn(parse(None, source.clone(), parses.clone()));
        // The second worker waits for the first's parse rather than starting its own.
        std::thread::sleep(std::time::Duration::from_millis(50));
        assert_eq!(parses.load(Ordering::SeqCst), 1);
        parse_may_finish.wait();
        let first = first.join().unwrap();
        let second = second.join().unwrap();
        assert!(Arc::ptr_eq(&first, &second));
        assert_eq!(parses.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn worker_diagnostics_visit_the_native_graph() {
        use super::{FfiDeclarationRejection, FfiSyntaxDeclaration, FfiSyntaxDiagnostic};
        use crate::css::ffi_support::FfiUtf16View;
        let parsed = std::thread::spawn(|| {
            let source = utf16("@未来 {} @media all { .目 { height: 13px; & {} 未知: 雪; width: 赤; } }");
            // SAFETY: The parse hands over one strong reference.
            let parse = unsafe {
                std::sync::Arc::from_raw(super::rust_parse_css_stylesheet_syntax(
                    FfiUtf16View::from_utf16(&source),
                    Some(&parse_context()),
                ))
            };
            assert_eq!(crate::css::ffi_stats::THREAD_UNSAFE_CPP_CALLBACK_COUNT.get(), 0);
            parse
        })
        .join()
        .unwrap();
        assert_eq!(parsed.declaration_errors.len(), 2);
        let names: Vec<_> = parsed
            .declaration_errors
            .iter()
            .map(|&index| {
                let declaration = &parsed.declarations[index as usize];
                parsed.values[declaration.name_offset..declaration.name_offset + declaration.name_length].to_vec()
            })
            .collect();
        assert_eq!(names, [utf16("未知"), utf16("width")]);
        let parse = parsed;
        extern "C" fn diagnostic(values: *const u16, count: usize, diagnostic: &FfiSyntaxDiagnostic) {
            let values = unsafe { ak::slice_from_raw_parts(values, count) };
            assert_eq!(diagnostic.code, FfiSyntaxDiagnosticCode::UnknownRule);
            assert_eq!(
                &values[diagnostic.primary_offset..diagnostic.primary_offset + diagnostic.primary_length],
                utf16("@未来")
            );
        }
        extern "C" fn declaration(values: *const u16, count: usize, declaration: &FfiSyntaxDeclaration) {
            let values = unsafe { ak::slice_from_raw_parts(values, count) };
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
        let rules = unsafe {
            std::rc::Rc::from_raw(super::rust_css_syntax_native_rules(
                crate::css::ffi_support::ArcRef::new(&parse),
            ))
        };
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
                FfiUtf16View::from_utf16(&source),
                ak::Span::new(&contexts),
                Some(&parse_context()),
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
            let values = unsafe { ak::slice_from_raw_parts(values, count) };
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
            b"a { width: 13.25px } @property --size { syntax: '<length>'; inherits: false; initial-value: 7.25px }",
        );
        let values = parse
            .rules
            .iter()
            .filter_map(|rule| rule.declaration_block())
            .flat_map(|block| block.properties().values())
            .chain(
                parse
                    .rules
                    .iter()
                    .filter_map(|rule| rule.property_rule()?.initial_value.as_ref()),
            )
            .filter(|value| matches!(value.as_ref(), crate::css::style_value::StyleValueData::Length { .. }))
            .cloned()
            .collect::<Vec<_>>();
        assert!(values.len() >= 2);
        let syntaxes = parse
            .rules
            .iter()
            .filter_map(|rule| rule.property_rule().map(|rule| &rule.syntax))
            .map(std::sync::Arc::downgrade)
            .collect::<Vec<_>>();
        assert!(!syntaxes.is_empty());
        drop(parse);
        assert!(
            values
                .iter()
                .all(|value| crate::css::style_value_arc::StyleValueArc::strong_count(value) == 1)
        );
        assert!(syntaxes.iter().all(|syntax| syntax.upgrade().is_none()));
    }

    #[test]
    fn consumers_can_retain_the_same_parsed_value_independently() {
        let parse = parse_test_stylesheet(b"a { width: 13.25px }");
        let pointer = crate::css::style_value_arc::StyleValueArc::as_ptr(
            &parse
                .root(0)
                .declaration_block()
                .unwrap()
                .properties()
                .get(0)
                .unwrap()
                .value,
        );
        let retain = || unsafe {
            crate::css::style_value_arc::StyleValueArc::increment_strong_count(pointer);
            crate::css::style_value_arc::StyleValueArc::from_raw(pointer)
        };
        let first = retain();
        let second = retain();
        drop(parse);
        assert!(crate::css::style_value_arc::StyleValueArc::ptr_eq(&first, &second));
        drop(first);
        assert!(matches!(
            *second,
            crate::css::style_value::StyleValueData::Length { value: 13.25, .. }
        ));
    }

    #[test]
    fn exposes_token_diagnostics_with_source_spans() {
        let parse = super::parse_stylesheet_uncached(
            b"a { color: \"bad\n; background: url(foo\"bar) }".as_slice().into(),
            None,
        );
        assert_eq!(parse.diagnostics.len(), 2);
        assert_eq!(parse.diagnostics[0].code, FfiSyntaxDiagnosticCode::BadString);
        assert_eq!(
            (parse.diagnostics[0].start_line, parse.diagnostics[0].start_column),
            (0, 11)
        );
        assert_eq!(
            (parse.diagnostics[0].end_line, parse.diagnostics[0].end_column),
            (0, 15)
        );
        assert_eq!(parse.diagnostics[1].code, FfiSyntaxDiagnosticCode::BadUrl);
        assert_eq!(
            (parse.diagnostics[1].start_line, parse.diagnostics[1].start_column),
            (1, 14)
        );
        assert_eq!(
            (parse.diagnostics[1].end_line, parse.diagnostics[1].end_column),
            (1, 26)
        );
    }

    #[test]
    fn classifies_structurally_invalid_and_misplaced_rules() {
        let parse = parse_test_stylesheet(
            b"undeclared|element {} @font-face serif {} @import url('data:text/css,'); \
              @namespace known 'urn:known'; known|element {}",
        );
        let root_kinds = parse
            .roots
            .iter()
            .map(|&index| parse.rule(index).rule_kind)
            .collect::<Vec<_>>();
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
        assert_eq!(parse.root(1).rule_kind, ParsedRuleKind::Invalid);
        assert_eq!(parse.root(2).rule_kind, ParsedRuleKind::Invalid);
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
        let root_kinds = parse
            .roots
            .iter()
            .map(|&index| parse.rule(index).rule_kind)
            .collect::<Vec<_>>();
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
        let ParsedRuleChild::Rule(media_child) = parse.children_of(parse.root(0))[0] else {
            panic!("expected rule")
        };
        assert_eq!(parse.rule(media_child).rule_kind, ParsedRuleKind::Invalid);
        let ParsedRuleChild::Rule(font_feature_values_child) = parse.children_of(parse.root(1))[0] else {
            panic!("expected rule")
        };
        assert_eq!(
            parse.rule(font_feature_values_child).rule_kind,
            ParsedRuleKind::FontFeatureValuesRule
        );
    }

    #[test]
    fn validates_nested_rule_contexts_without_flattening_them() {
        let parse = parse_test_stylesheet(b"a { @layer direct; } @scope { @layer scoped; }");
        let layer_rules = parse
            .roots
            .iter()
            .flat_map(|&index| parse.children_of(parse.rule(index)))
            .filter_map(|child| match *child {
                ParsedRuleChild::Rule(rule) => Some(parse.rule(rule)),
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
    fn expands_and_deduplicates_descriptors_in_last_valid_order() {
        use crate::css::descriptor_metadata::descriptor_metadata;

        let sheet = parse_test_stylesheet(
            b"@page { margin: 1px 2px 3px 4px; @top-left { content: 'page'; } margin-right: 5px; size: a4; size: invalid; }",
        );
        let descriptors = &sheet.root(0).descriptor_block().unwrap().descriptors;
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
    fn parses_keyframe_selectors_with_an_unterminated_comment() {
        let values = consume_a_list_of_component_values(tokenize_for_parser(b"50% /*".as_slice())).unwrap();
        assert_eq!(
            parse_keyframe_selectors(&values),
            ParsedRulePrelude::KeyframeSelectors(vec![50.0])
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
