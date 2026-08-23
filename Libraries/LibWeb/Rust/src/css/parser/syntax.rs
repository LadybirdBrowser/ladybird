/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Parsed values use the thread-confined shared graph owned by the C++ style objects.
#![allow(clippy::arc_with_non_send_sync)]

use crate::css::css_enums::keyword_from_ascii_case_insensitive;
use crate::css::css_tokenizer::tokenize_for_parser;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::arbitrary_substitution::{
    SubstitutionFunctionsPresence, collect_arbitrary_substitution_function_presence, declaration_value_is_valid,
};
use crate::css::parser::color_parser::parse_color_value;
use crate::css::parser::component_value::{ComponentValue, consume_a_list_of_component_values};
use crate::css::parser::images_gradients_parser::parse_image_value;
use crate::css::parser::token_stream::TokenStream;
use crate::css::parser::transforms_effects_parser::{parse_transform, parse_transform_function};
use crate::css::parser::value_parser::{
    FfiParseStatus, ParseContext, is_valid_custom_ident, parse_custom_ident_value, parse_string_value,
    parse_syntax_numeric_value, parse_url_value, retain_fly_string, unresolved_value, value_list,
};
use crate::css::property_metadata::property_id;
use crate::css::style_value::StyleValueData;
use std::ffi::c_void;
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SyntaxType {
    Angle,
    Color,
    CustomIdent,
    Image,
    Integer,
    Length,
    LengthPercentage,
    Number,
    Percentage,
    Resolution,
    String,
    Time,
    TransformFunction,
    TransformList,
    Url,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SyntaxMultiplier {
    SpaceSeparated,
    CommaSeparated,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum SyntaxNode {
    Universal,
    Ident {
        value: Box<[u16]>,
        case_sensitive: bool,
    },
    Type(SyntaxType),
    Multiplier {
        child: Box<SyntaxNode>,
        multiplier: SyntaxMultiplier,
    },
    Alternatives(Box<[SyntaxNode]>),
}

struct ParsedSyntax {
    nodes: Vec<ParsedSyntaxNode>,
    children: Vec<usize>,
    values: Vec<u16>,
    root: usize,
    consumed_component_values: usize,
}

struct ParsedSyntaxNode {
    node_type: u8,
    case_sensitive: bool,
    value_start: usize,
    value_length: usize,
    children_start: usize,
    child_count: usize,
}

fn flatten_syntax_node(
    syntax: &SyntaxNode,
    nodes: &mut Vec<ParsedSyntaxNode>,
    children: &mut Vec<usize>,
    values: &mut Vec<u16>,
) -> usize {
    let (node_type, case_sensitive, value, child_nodes): (u8, bool, &[u16], Vec<&SyntaxNode>) = match syntax {
        SyntaxNode::Universal => (0, false, &[], vec![]),
        SyntaxNode::Ident { value, case_sensitive } => (1, *case_sensitive, value, vec![]),
        SyntaxNode::Type(syntax_type) => {
            let value = match syntax_type {
                SyntaxType::Angle => b"angle".as_slice(),
                SyntaxType::Color => b"color",
                SyntaxType::CustomIdent => b"custom-ident",
                SyntaxType::Image => b"image",
                SyntaxType::Integer => b"integer",
                SyntaxType::Length => b"length",
                SyntaxType::LengthPercentage => b"length-percentage",
                SyntaxType::Number => b"number",
                SyntaxType::Percentage => b"percentage",
                SyntaxType::Resolution => b"resolution",
                SyntaxType::String => b"string",
                SyntaxType::Time => b"time",
                SyntaxType::TransformFunction => b"transform-function",
                SyntaxType::TransformList => b"transform-list",
                SyntaxType::Url => b"url",
            };
            let value_start = values.len();
            values.extend(value.iter().map(|&byte| u16::from(byte)));
            let node_index = nodes.len();
            nodes.push(ParsedSyntaxNode {
                node_type: 2,
                case_sensitive: false,
                value_start,
                value_length: value.len(),
                children_start: 0,
                child_count: 0,
            });
            return node_index;
        }
        SyntaxNode::Multiplier { child, multiplier } => (
            if *multiplier == SyntaxMultiplier::SpaceSeparated {
                3
            } else {
                4
            },
            false,
            &[],
            vec![child],
        ),
        SyntaxNode::Alternatives(alternatives) => (5, false, &[], alternatives.iter().collect()),
    };

    let child_indices: Vec<_> = child_nodes
        .into_iter()
        .map(|child| flatten_syntax_node(child, nodes, children, values))
        .collect();
    let children_start = children.len();
    children.extend(child_indices);
    let value_start = values.len();
    values.extend_from_slice(value);
    let node_index = nodes.len();
    nodes.push(ParsedSyntaxNode {
        node_type,
        case_sensitive,
        value_start,
        value_length: value.len(),
        children_start,
        child_count: children.len() - children_start,
    });
    node_index
}

fn parsed_syntax(syntax: SyntaxNode, consumed_component_values: usize) -> ParsedSyntax {
    let mut nodes = Vec::new();
    let mut children = Vec::new();
    let mut values = Vec::new();
    let root = flatten_syntax_node(&syntax, &mut nodes, &mut children, &mut values);
    ParsedSyntax {
        nodes,
        children,
        values,
        root,
        consumed_component_values,
    }
}

fn equals_ascii(value: &[u16], expected: &[u8]) -> bool {
    value.len() == expected.len()
        && value
            .iter()
            .zip(expected)
            .all(|(&left, &right)| u8::try_from(left) == Ok(right))
}

fn syntax_type(name: &[u16]) -> Option<SyntaxType> {
    [
        (b"angle".as_slice(), SyntaxType::Angle),
        (b"color".as_slice(), SyntaxType::Color),
        (b"custom-ident".as_slice(), SyntaxType::CustomIdent),
        (b"image".as_slice(), SyntaxType::Image),
        (b"integer".as_slice(), SyntaxType::Integer),
        (b"length".as_slice(), SyntaxType::Length),
        (b"length-percentage".as_slice(), SyntaxType::LengthPercentage),
        (b"number".as_slice(), SyntaxType::Number),
        (b"percentage".as_slice(), SyntaxType::Percentage),
        (b"resolution".as_slice(), SyntaxType::Resolution),
        (b"string".as_slice(), SyntaxType::String),
        (b"time".as_slice(), SyntaxType::Time),
        (b"url".as_slice(), SyntaxType::Url),
        (b"transform-function".as_slice(), SyntaxType::TransformFunction),
    ]
    .into_iter()
    .find_map(|(expected, syntax_type)| equals_ascii(name, expected).then_some(syntax_type))
}

fn discard_whitespace(values: &[ComponentValue], position: &mut usize) {
    while values.get(*position).is_some_and(ComponentValue::is_whitespace) {
        *position += 1;
    }
}

fn parse_single_component(
    values: &[ComponentValue],
    position: &mut usize,
    limit_ident_to_custom_ident: bool,
) -> Option<SyntaxNode> {
    let start = *position;
    discard_whitespace(values, position);

    if let Some(identifier) = values.get(*position).and_then(ComponentValue::ident) {
        *position += 1;
        if limit_ident_to_custom_ident && !is_valid_custom_ident(identifier, &[]) {
            *position = start;
            return None;
        }
        return Some(SyntaxNode::Ident {
            value: identifier.into(),
            case_sensitive: limit_ident_to_custom_ident,
        });
    }

    if values.get(*position).is_some_and(|value| value.is_delim(b'<')) {
        *position += 1;
        let name = values.get(*position).and_then(ComponentValue::ident);
        *position += usize::from(name.is_some());
        if let Some(name) = name
            && values.get(*position).is_some_and(|value| value.is_delim(b'>'))
            && let Some(syntax_type) = syntax_type(name)
        {
            *position += 1;
            return Some(SyntaxNode::Type(syntax_type));
        }
    }

    *position = start;
    None
}

fn parse_component(
    values: &[ComponentValue],
    position: &mut usize,
    limit_ident_to_custom_ident: bool,
) -> Option<SyntaxNode> {
    let start = *position;
    discard_whitespace(values, position);

    if values.get(*position).is_some_and(|value| value.is_delim(b'<'))
        && values
            .get(*position + 1)
            .and_then(ComponentValue::ident)
            .is_some_and(|name| equals_ascii(name, b"transform-list"))
        && values.get(*position + 2).is_some_and(|value| value.is_delim(b'>'))
    {
        *position += 3;
        return Some(SyntaxNode::Type(SyntaxType::TransformList));
    }

    let child = parse_single_component(values, position, limit_ident_to_custom_ident)?;
    let multiplier = if values.get(*position).is_some_and(|value| value.is_delim(b'#')) {
        Some(SyntaxMultiplier::CommaSeparated)
    } else if values.get(*position).is_some_and(|value| value.is_delim(b'+')) {
        Some(SyntaxMultiplier::SpaceSeparated)
    } else {
        None
    };
    if let Some(multiplier) = multiplier {
        *position += 1;
        return Some(SyntaxNode::Multiplier {
            child: Box::new(child),
            multiplier,
        });
    }
    if *position == start {
        return None;
    }
    Some(child)
}

// https://drafts.csswg.org/css-values-5/#typedef-syntax
pub(crate) fn parse_syntax(source: &[u16], limit_ident_to_custom_ident: bool) -> Option<SyntaxNode> {
    let values = consume_a_list_of_component_values(tokenize_for_parser(
        crate::css::css_tokenizer::TokenizerInput::Utf16(source),
    ))
    .ok()?;
    parse_syntax_values(&values, limit_ident_to_custom_ident)
}

fn parse_syntax_values(values: &[ComponentValue], limit_ident_to_custom_ident: bool) -> Option<SyntaxNode> {
    let mut position = 0;
    discard_whitespace(values, &mut position);

    if values.get(position).is_some_and(|value| value.is_delim(b'*')) {
        position += 1;
        discard_whitespace(values, &mut position);
        return (position == values.len()).then_some(SyntaxNode::Universal);
    }

    if let Some(string) = values.get(position).and_then(ComponentValue::string) {
        position += 1;
        discard_whitespace(values, &mut position);
        if position != values.len() {
            return None;
        }
        return parse_syntax(string, limit_ident_to_custom_ident);
    }

    let mut components = vec![parse_component(values, &mut position, limit_ident_to_custom_ident)?];
    discard_whitespace(values, &mut position);
    while position < values.len() {
        if !values.get(position).is_some_and(|value| value.is_delim(b'|')) {
            return None;
        }
        position += 1;
        discard_whitespace(values, &mut position);
        components.push(parse_component(values, &mut position, limit_ident_to_custom_ident)?);
        discard_whitespace(values, &mut position);
    }

    match components.as_slice() {
        [_] => components.pop(),
        _ => Some(SyntaxNode::Alternatives(components.into_boxed_slice())),
    }
}

fn identifier_matches(value: &[u16], expected: &[u16], case_sensitive: bool) -> bool {
    value.len() == expected.len()
        && value.iter().zip(expected).all(|(&left, &right)| {
            left == right
                || (!case_sensitive
                    && u8::try_from(left)
                        .ok()
                        .zip(u8::try_from(right).ok())
                        .is_some_and(|(left, right)| left.eq_ignore_ascii_case(&right)))
        })
}

fn parse_type(context: &ParseContext, syntax_type: SyntaxType, tokens: &mut TokenStream<'_>) -> Option<StyleValueData> {
    tokens.discard_whitespace();
    let parsed = match syntax_type {
        SyntaxType::Color => parse_color_value(context, property_id::CUSTOM, tokens, false),
        SyntaxType::CustomIdent => {
            let value = parse_custom_ident_value(context, tokens.next_token(), &[])?;
            tokens.discard_a_token();
            Some(value)
        }
        SyntaxType::Image => parse_image_value(context, property_id::CUSTOM, tokens, false),
        SyntaxType::String => {
            let value = parse_string_value(context, tokens.next_token())?;
            tokens.discard_a_token();
            Some(value)
        }
        SyntaxType::TransformFunction => {
            let value = parse_transform_function(context, property_id::CUSTOM, tokens.next_token())?;
            tokens.discard_a_token();
            Some(value)
        }
        SyntaxType::TransformList => {
            let start = tokens.current_index();
            while tokens.has_next_token() {
                tokens.discard_a_token();
            }
            parse_transform(context, property_id::CUSTOM, tokens.tokens_since(start))
        }
        SyntaxType::Url => {
            let value = parse_url_value(context, tokens.next_token())?;
            tokens.discard_a_token();
            Some(value)
        }
        _ => {
            let value = parse_syntax_numeric_value(context, syntax_type, tokens.next_token())?;
            tokens.discard_a_token();
            Some(value)
        }
    }?;
    Some(parsed)
}

fn parse_node(context: &ParseContext, syntax: &SyntaxNode, tokens: &mut TokenStream<'_>) -> Option<StyleValueData> {
    let mut transaction = tokens.begin_transaction();
    let parsed = match syntax {
        SyntaxNode::Universal => return None,
        SyntaxNode::Ident { value, case_sensitive } => {
            transaction.discard_whitespace();
            let identifier = transaction.consume_a_token().ident()?;
            if !identifier_matches(identifier, value, *case_sensitive) {
                return None;
            }
            if let Some(keyword) = keyword_from_ascii_case_insensitive(value) {
                StyleValueData::Keyword { keyword }
            } else {
                StyleValueData::CustomIdent {
                    custom_ident: retain_fly_string(context, value)?,
                }
            }
        }
        SyntaxNode::Type(syntax_type) => parse_type(context, *syntax_type, &mut transaction)?,
        SyntaxNode::Multiplier { child, multiplier } => {
            let mut values = Vec::new();
            transaction.discard_whitespace();
            loop {
                values.push(parse_node(context, child, &mut transaction)?);
                transaction.discard_whitespace();
                match multiplier {
                    SyntaxMultiplier::CommaSeparated if transaction.next_token().is_comma() => {
                        transaction.discard_a_token();
                        transaction.discard_whitespace();
                        if !transaction.has_next_token() {
                            return None;
                        }
                    }
                    SyntaxMultiplier::SpaceSeparated if transaction.has_next_token() => {}
                    _ => break,
                }
            }
            value_list(values, u8::from(*multiplier == SyntaxMultiplier::CommaSeparated), true)
        }
        SyntaxNode::Alternatives(alternatives) => {
            for alternative in alternatives {
                let parsed = {
                    let mut alternative_transaction = transaction.begin_transaction();
                    if let Some(parsed) = parse_node(context, alternative, &mut alternative_transaction) {
                        alternative_transaction.discard_whitespace();
                        if !alternative_transaction.has_next_token() {
                            alternative_transaction.commit();
                            Some(parsed)
                        } else {
                            None
                        }
                    } else {
                        None
                    }
                };
                if let Some(parsed) = parsed {
                    transaction.commit();
                    return Some(parsed);
                }
            }
            return None;
        }
    };
    transaction.commit();
    Some(parsed)
}

// https://drafts.csswg.org/css-values-5/#parse-with-a-syntax
pub(crate) fn parse_with_syntax(context: &ParseContext, source: &[u16], syntax: &SyntaxNode) -> Option<StyleValueData> {
    let values = consume_a_list_of_component_values(tokenize_for_parser(
        crate::css::css_tokenizer::TokenizerInput::Utf16(source),
    ))
    .ok()?;
    if matches!(syntax, SyntaxNode::Universal) {
        if !declaration_value_is_valid(&values) {
            return None;
        }
        let mut presence = SubstitutionFunctionsPresence::default();
        collect_arbitrary_substitution_function_presence(&values, &mut presence).ok()?;
        return Some(unresolved_value(source, &[], presence));
    }
    let mut tokens = TokenStream::new(&values);
    let parsed = parse_node(context, syntax, &mut tokens)?;
    tokens.discard_whitespace();
    (!tokens.has_next_token()).then_some(parsed)
}

/// Parses a complete CSS syntax definition and returns an opaque syntax tree.
///
/// # Safety
/// `source` must remain readable for `source_length` UTF-16 code units during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_syntax(source: FfiUtf16View, limit_ident_to_custom_ident: bool) -> *mut c_void {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.to_utf16() }) else {
            return std::ptr::null_mut();
        };
        let Some(syntax) = parse_syntax(&source, limit_ident_to_custom_ident) else {
            return std::ptr::null_mut();
        };
        Box::into_raw(Box::new(parsed_syntax(syntax, 0))).cast()
    })
}

/// Parses one CSS syntax component and returns an opaque syntax tree.
///
/// # Safety
/// `source` must remain readable for `source_length` UTF-16 code units during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_syntax_component(
    source: FfiUtf16View,
    limit_ident_to_custom_ident: bool,
) -> *mut c_void {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let Some(values) = consume_a_list_of_component_values(tokenize_for_parser(source)).ok() else {
            return std::ptr::null_mut();
        };
        let mut position = 0;
        let Some(syntax) = parse_component(&values, &mut position, limit_ident_to_custom_ident) else {
            return std::ptr::null_mut();
        };
        Box::into_raw(Box::new(parsed_syntax(syntax, position))).cast()
    })
}

/// # Safety
/// `syntax` must be a live handle returned by `rust_parse_syntax` or `rust_parse_syntax_component`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_syntax_root(syntax: *const c_void) -> usize {
    crate::abort_on_panic(|| unsafe { &*syntax.cast::<ParsedSyntax>() }.root)
}

/// # Safety
/// `syntax` must be a live parsed-syntax handle and `node` must identify one of its nodes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_syntax_node_type(syntax: *const c_void, node: usize) -> u8 {
    crate::abort_on_panic(|| unsafe { &*syntax.cast::<ParsedSyntax>() }.nodes[node].node_type)
}

/// # Safety
/// `syntax` and `node` must be valid as for `rust_syntax_node_type`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_syntax_node_is_case_sensitive(syntax: *const c_void, node: usize) -> bool {
    crate::abort_on_panic(|| unsafe { &*syntax.cast::<ParsedSyntax>() }.nodes[node].case_sensitive)
}

/// # Safety
/// `syntax` and `node` must be valid as for `rust_syntax_node_type`. `out_length` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_syntax_node_value(
    syntax: *const c_void,
    node: usize,
    out_length: *mut usize,
) -> *const u16 {
    crate::abort_on_panic(|| {
        let syntax = unsafe { &*syntax.cast::<ParsedSyntax>() };
        let node = &syntax.nodes[node];
        unsafe { *out_length = node.value_length };
        syntax.values[node.value_start..].as_ptr()
    })
}

/// # Safety
/// `syntax` and `node` must be valid as for `rust_syntax_node_type`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_syntax_node_child_count(syntax: *const c_void, node: usize) -> usize {
    crate::abort_on_panic(|| unsafe { &*syntax.cast::<ParsedSyntax>() }.nodes[node].child_count)
}

/// # Safety
/// `syntax` and `node` must be valid as for `rust_syntax_node_type`, and `child` must be in range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_syntax_node_child(syntax: *const c_void, node: usize, child: usize) -> usize {
    crate::abort_on_panic(|| {
        let syntax = unsafe { &*syntax.cast::<ParsedSyntax>() };
        let node = &syntax.nodes[node];
        syntax.children[node.children_start + child]
    })
}

/// # Safety
/// `syntax` must be a live parsed-syntax handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_syntax_consumed_component_values(syntax: *const c_void) -> usize {
    crate::abort_on_panic(|| unsafe { &*syntax.cast::<ParsedSyntax>() }.consumed_component_values)
}

/// # Safety
/// `syntax` must be null or a live parsed-syntax handle, and must only be freed once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_syntax_free(syntax: *mut c_void) {
    crate::abort_on_panic(|| {
        if !syntax.is_null() {
            drop(unsafe { Box::from_raw(syntax.cast::<ParsedSyntax>()) });
        }
    });
}

/// Parses a CSS value against a registered syntax and returns one strong style-value handle.
///
/// # Safety
/// All non-null pointers must remain readable for their accompanying lengths during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_with_syntax(
    context: *const ParseContext,
    source: FfiUtf16View,
    syntax: FfiUtf16View,
    limit_ident_to_custom_ident: bool,
    out_status: *mut FfiParseStatus,
) -> *const c_void {
    crate::abort_on_panic(|| {
        if context.is_null() || out_status.is_null() {
            return std::ptr::null();
        }
        let Some(source) = (unsafe { source.to_utf16() }) else {
            unsafe { *out_status = FfiParseStatus::Invalid };
            return std::ptr::null();
        };
        let Some(syntax) = (unsafe { syntax.to_utf16() }) else {
            unsafe { *out_status = FfiParseStatus::Invalid };
            return std::ptr::null();
        };
        let Some(syntax) = parse_syntax(&syntax, limit_ident_to_custom_ident) else {
            unsafe { *out_status = FfiParseStatus::Invalid };
            return std::ptr::null();
        };
        let Some(parsed) = parse_with_syntax(unsafe { &*context }, &source, &syntax) else {
            unsafe { *out_status = FfiParseStatus::Invalid };
            return std::ptr::null();
        };
        unsafe { *out_status = FfiParseStatus::Parsed };
        Arc::into_raw(Arc::new(parsed)).cast()
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn context(random_function_index: &mut usize) -> ParseContext {
        ParseContext {
            in_quirks_mode: false,
            is_svg_presentation_attribute: false,
            is_substituted_value: false,
            contains_attr_tainted_values: false,
            value_contexts: std::ptr::null(),
            value_context_count: 0,
            document_url: std::ptr::null(),
            document_url_length: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            intern_utf16_fly_string: None,
            normalize_svg_path_data: None,
            precomputed_svg_paths: std::ptr::null(),
            precomputed_svg_path_count: 0,
            font_format_is_supported: None,
            font_tech_is_supported: None,
            random_function_index,
        }
    }

    #[test]
    fn parses_supported_syntax_forms() {
        assert_eq!(parse_syntax(&utf16(" * "), true), Some(SyntaxNode::Universal));
        assert_eq!(
            parse_syntax(&utf16("<length> | auto | <color>#"), true),
            Some(SyntaxNode::Alternatives(
                vec![
                    SyntaxNode::Type(SyntaxType::Length),
                    SyntaxNode::Ident {
                        value: "auto".encode_utf16().collect(),
                        case_sensitive: true,
                    },
                    SyntaxNode::Multiplier {
                        child: Box::new(SyntaxNode::Type(SyntaxType::Color)),
                        multiplier: SyntaxMultiplier::CommaSeparated,
                    },
                ]
                .into_boxed_slice(),
            ))
        );
        assert_eq!(
            parse_syntax(&utf16(r#""<number>+""#), false),
            Some(SyntaxNode::Multiplier {
                child: Box::new(SyntaxNode::Type(SyntaxType::Number)),
                multiplier: SyntaxMultiplier::SpaceSeparated,
            })
        );
    }

    #[test]
    fn rejects_invalid_syntax() {
        for source in [
            b"".as_slice(),
            b"<unknown>",
            b"<transform-list>+",
            b"<length> +",
            b"<length> || <color>",
            b"<length> |",
            b"inherit",
        ] {
            assert_eq!(
                parse_syntax(&utf16(std::str::from_utf8(source).unwrap()), true),
                None,
                "{}",
                String::from_utf8_lossy(source)
            );
        }
    }

    #[test]
    fn bare_ident_case_sensitivity_depends_on_caller() {
        let Some(SyntaxNode::Ident { case_sensitive, .. }) = parse_syntax(&utf16("Auto"), true) else {
            panic!("expected identifier syntax");
        };
        assert!(case_sensitive);
        let Some(SyntaxNode::Ident { case_sensitive, .. }) = parse_syntax(&utf16("Auto"), false) else {
            panic!("expected identifier syntax");
        };
        assert!(!case_sensitive);
    }

    #[test]
    fn parses_values_against_syntax() {
        let mut random_function_index = 0;
        let context = context(&mut random_function_index);
        let syntax = parse_syntax(&utf16("<length> | <number>#"), true).unwrap();
        assert!(parse_with_syntax(&context, &utf16("12px"), &syntax).is_some());
        assert!(parse_with_syntax(&context, &utf16("1, 2, calc(1 + 2)"), &syntax).is_some());
        assert!(parse_with_syntax(&context, &utf16("red"), &syntax).is_none());
        assert!(parse_with_syntax(&context, &utf16("1,"), &syntax).is_none());
    }

    fn utf16(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }
}
