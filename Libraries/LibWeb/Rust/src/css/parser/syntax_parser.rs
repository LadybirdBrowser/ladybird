/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_tokenizer::{
    ParserString, ParserToken, ParserTokenKind, SourcePosition, TokenizerInput, tokenize_for_parser,
};
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::component_value::{ComponentKind, ComponentValue, consume_a_component_value};
use crate::css::parser::value_parser::{
    FfiParseStatus, FfiValueParsingContext, FfiValueParsingContextKind, ParseContext, ParseOutcome,
    parse_css_value_with_utf16_source,
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
    fn new<'a>(source: impl Into<TokenizerInput<'a>>, rule_context: Vec<RuleContext>) -> Self {
        Self {
            tokens: tokenize_for_parser(source),
            position: 0,
            rule_context,
        }
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
        Some(Declaration {
            name,
            value,
            important,
            original_value_text,
            original_full_text,
            source_position: token.start_position,
            is_property: self.declaration_is_property(),
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

pub(crate) fn parse_stylesheet<'a>(source: impl Into<TokenizerInput<'a>>) -> Vec<Rule> {
    Parser::new(source, Vec::new()).consume_stylesheet_contents()
}

pub(crate) fn parse_block_contents<'a>(
    source: impl Into<TokenizerInput<'a>>,
    rule_context: Vec<RuleContext>,
) -> Vec<RuleOrDeclarations> {
    Parser::new(source, rule_context).consume_block_contents()
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
    parse_context: *const ParseContext,
    resolve_property_id: Option<unsafe extern "C" fn(*const u16, usize) -> u16>,
    retain_property_components: bool,
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
    fn new(
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
            parse_context,
            resolve_property_id,
            retain_property_components: std::env::var("LIBWEB_VERIFY_RUST_SYNTAX_PARSER").as_deref() == Ok("1"),
            preserve_property_source_text,
        }
    }

    fn append_value(&mut self, value: &[u16]) -> (usize, usize) {
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

    fn append_component_list(&mut self, values: &[ComponentValue]) -> (usize, usize) {
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
        let (values_start, value_count) = if declaration.is_property && !self.retain_property_components {
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
        if !declaration.is_property || self.parse_context.is_null() {
            return (u16::MAX, FfiParseStatus::NotHandled, std::ptr::null());
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
            ParseOutcome::NotHandled(_) => (property_id, FfiParseStatus::NotHandled, std::ptr::null()),
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
        });
        index
    }

    fn append_roots(&mut self, rules: &[Rule]) {
        self.roots = rules.iter().map(|rule| self.append_rule(rule)).collect();
    }

    fn append_root_items(&mut self, items: &[RuleOrDeclarations]) {
        self.roots = items.iter().map(|item| self.append_item(item)).collect();
    }

    fn data(&self) -> FfiSyntaxParseData {
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
        }
    }
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
        let rules = parse_stylesheet(source);
        let mut parse = FfiSyntaxParse::new(parse_context, resolve_property_id, false);
        parse.append_roots(&rules);
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
        let items = parse_block_contents(source, contexts);
        let mut parse = FfiSyntaxParse::new(parse_context, resolve_property_id, preserve_property_source_text);
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
    use super::{Declaration, Rule, RuleContext, RuleOrDeclarations, parse_block_contents, parse_stylesheet};

    fn utf16(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
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
    fn consumes_declarations_and_important_like_cpp_oracle() {
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
}
