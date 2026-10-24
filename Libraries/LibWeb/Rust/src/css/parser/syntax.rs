/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::keyword_from_ascii_case_insensitive;
use crate::css::css_tokenizer::tokenize_for_parser;
use crate::css::ffi_support::{FfiUtf16View, OwnedArc};
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
    parse_syntax_numeric_value, parse_url_value, unresolved_value, unresolved_value_from_component_values, value_list,
};
use crate::css::property_metadata::property_id;
use crate::css::serialize::{StringUnits, TextSink, serialize_an_identifier};
use crate::css::style_value::{CssString, StyleValueData};
use std::ffi::c_void;
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SyntaxType {
    Angle,
    Color,
    CustomIdent,
    /// Not a syntax a registration names: a `<style-range-value>` may be a `<frequency>`.
    Frequency,
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

fn serialize_syntax(syntax: &SyntaxNode, sink: &mut TextSink) {
    match syntax {
        SyntaxNode::Universal => sink.push_ascii("*"),
        SyntaxNode::Ident { value, .. } => serialize_an_identifier(sink, &StringUnits::Utf16(value)),
        SyntaxNode::Type(syntax_type) => {
            sink.push_ascii("<");
            sink.push_ascii(match syntax_type {
                SyntaxType::Angle => "angle",
                SyntaxType::Color => "color",
                SyntaxType::CustomIdent => "custom-ident",
                SyntaxType::Frequency => "frequency",
                SyntaxType::Image => "image",
                SyntaxType::Integer => "integer",
                SyntaxType::Length => "length",
                SyntaxType::LengthPercentage => "length-percentage",
                SyntaxType::Number => "number",
                SyntaxType::Percentage => "percentage",
                SyntaxType::Resolution => "resolution",
                SyntaxType::String => "string",
                SyntaxType::Time => "time",
                SyntaxType::TransformFunction => "transform-function",
                SyntaxType::TransformList => "transform-list",
                SyntaxType::Url => "url",
            });
            sink.push_ascii(">");
        }
        SyntaxNode::Multiplier { child, multiplier } => {
            serialize_syntax(child, sink);
            sink.push_ascii(if *multiplier == SyntaxMultiplier::SpaceSeparated {
                "+"
            } else {
                "#"
            });
        }
        SyntaxNode::Alternatives(alternatives) => {
            for (index, alternative) in alternatives.iter().enumerate() {
                if index > 0 {
                    sink.push_ascii(" | ");
                }
                serialize_syntax(alternative, sink);
            }
        }
    }
}

fn is_single_syntax_component(syntax: &SyntaxNode) -> bool {
    match syntax {
        SyntaxNode::Universal | SyntaxNode::Ident { .. } | SyntaxNode::Type(_) => true,
        SyntaxNode::Multiplier { child, .. } => {
            matches!(child.as_ref(), SyntaxNode::Ident { .. } | SyntaxNode::Type(_))
        }
        SyntaxNode::Alternatives(_) => false,
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

pub(crate) fn parse_syntax_values(values: &[ComponentValue], limit_ident_to_custom_ident: bool) -> Option<SyntaxNode> {
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

pub(crate) fn parse_syntax_component_values(
    values: &[ComponentValue],
    limit_ident_to_custom_ident: bool,
) -> Option<usize> {
    let mut position = 0;
    parse_component(values, &mut position, limit_ident_to_custom_ident)?;
    Some(position)
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
    match syntax_type {
        SyntaxType::Color => parse_color_value(context, property_id::CUSTOM, tokens, false),
        SyntaxType::CustomIdent => parse_custom_ident_value(context, tokens.consume_a_token(), &[]),
        SyntaxType::Image => parse_image_value(context, property_id::CUSTOM, tokens, false),
        SyntaxType::String => parse_string_value(context, tokens.consume_a_token()),
        SyntaxType::TransformFunction => {
            parse_transform_function(context, property_id::CUSTOM, tokens.consume_a_token())
        }
        SyntaxType::TransformList => {
            let start = tokens.current_index();
            while tokens.has_next_token() {
                tokens.discard_a_token();
            }
            parse_transform(context, property_id::CUSTOM, tokens.tokens_since(start))
        }
        SyntaxType::Url => parse_url_value(context, tokens.consume_a_token()),
        _ => parse_syntax_numeric_value(context, syntax_type, tokens.consume_a_token()),
    }
}

/// May consume tokens on failure. Callers that retry must use a transaction.
fn parse_node(context: &ParseContext, syntax: &SyntaxNode, tokens: &mut TokenStream<'_>) -> Option<StyleValueData> {
    match syntax {
        SyntaxNode::Universal => None,
        SyntaxNode::Ident { value, case_sensitive } => {
            tokens.discard_whitespace();
            let identifier = tokens.consume_a_token().ident()?;
            if !identifier_matches(identifier, value, *case_sensitive) {
                return None;
            }
            if let Some(keyword) = keyword_from_ascii_case_insensitive(value) {
                Some(StyleValueData::Keyword { keyword })
            } else {
                Some(StyleValueData::CustomIdent {
                    custom_ident: CssString::from_utf16(value),
                })
            }
        }
        SyntaxNode::Type(syntax_type) => parse_type(context, *syntax_type, tokens),
        SyntaxNode::Multiplier { child, multiplier } => {
            let mut values = Vec::new();
            tokens.discard_whitespace();
            loop {
                values.push(parse_node(context, child, tokens)?);
                tokens.discard_whitespace();
                match multiplier {
                    SyntaxMultiplier::CommaSeparated if tokens.next_token().is_comma() => {
                        tokens.discard_a_token();
                        tokens.discard_whitespace();
                        if !tokens.has_next_token() {
                            return None;
                        }
                    }
                    SyntaxMultiplier::SpaceSeparated if tokens.has_next_token() => {}
                    _ => break,
                }
            }
            Some(value_list(
                values,
                u8::from(*multiplier == SyntaxMultiplier::CommaSeparated),
                true,
            ))
        }
        SyntaxNode::Alternatives(alternatives) => {
            for alternative in alternatives {
                let mut transaction = tokens.begin_transaction();
                if let Some(parsed) = parse_node(context, alternative, &mut transaction) {
                    transaction.discard_whitespace();
                    if !transaction.has_next_token() {
                        transaction.commit();
                        return Some(parsed);
                    }
                }
            }
            None
        }
    }
}

// https://drafts.csswg.org/css-values-5/#parse-with-a-syntax
pub(crate) fn parse_with_syntax(context: &ParseContext, source: &[u16], syntax: &SyntaxNode) -> Option<StyleValueData> {
    let values = consume_a_list_of_component_values(tokenize_for_parser(
        crate::css::css_tokenizer::TokenizerInput::Utf16(source),
    ))
    .ok()?;
    parse_with_syntax_component_values(context, &values, syntax, Some(source))
}

/// Parses component values against a syntax. Original source text is only used
/// when constructing an unresolved value for the universal syntax; when absent,
/// the component values are serialized instead.
pub(crate) fn parse_with_syntax_component_values(
    context: &ParseContext,
    values: &[ComponentValue],
    syntax: &SyntaxNode,
    original_source: Option<&[u16]>,
) -> Option<StyleValueData> {
    if matches!(syntax, SyntaxNode::Universal) {
        if !declaration_value_is_valid(values) {
            return None;
        }

        let mut presence = SubstitutionFunctionsPresence::default();
        collect_arbitrary_substitution_function_presence(values, &mut presence).ok()?;

        return Some(match original_source {
            Some(source) => unresolved_value(source, &[], values.to_vec(), presence),
            None => unresolved_value_from_component_values(values.to_vec(), presence),
        });
    }

    let mut tokens = TokenStream::new(values);
    let parsed = parse_node(context, syntax, &mut tokens)?;
    tokens.discard_whitespace();
    (!tokens.has_next_token()).then_some(parsed)
}

/// Parses a complete CSS syntax definition and returns an opaque syntax tree.
#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_syntax(source: FfiUtf16View, limit_ident_to_custom_ident: bool) -> *const c_void {
    let Some(source) = source.to_utf16() else {
        return std::ptr::null();
    };
    let Some(syntax) = parse_syntax(&source, limit_ident_to_custom_ident) else {
        return std::ptr::null();
    };
    Arc::into_raw(Arc::new(syntax)).cast()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_syntax_retain(syntax: crate::css::ffi_support::ArcRef<'_, SyntaxNode>) -> *const SyntaxNode {
    Arc::into_raw(syntax.to_arc())
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_syntax_release(syntax: OwnedArc<SyntaxNode>) {
    drop(syntax);
}

/// # Safety
/// `syntax` must be null or a live parsed-syntax handle.
pub(crate) unsafe fn clone_syntax_handle(syntax: *const c_void) -> Option<SyntaxNode> {
    unsafe { syntax.cast::<SyntaxNode>().as_ref() }.cloned()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_syntax_is_universal(syntax: &SyntaxNode) -> bool {
    matches!(syntax, SyntaxNode::Universal)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_syntax_is_single_component(syntax: &SyntaxNode) -> bool {
    is_single_syntax_component(syntax)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_syntax_equals(left: &SyntaxNode, right: &SyntaxNode) -> bool {
    left == right
}

/// Serializes a syntax handle into an owned native `AK::Utf16String`, which C++ must adopt.
#[unsafe(no_mangle)]
pub extern "C" fn rust_syntax_serialize(syntax: &SyntaxNode) -> usize {
    let mut sink = TextSink::new();
    serialize_syntax(syntax, &mut sink);
    crate::css::serialize::sink_into_raw(sink)
}

/// Parses a CSS value against a registered syntax and returns one strong style-value handle.
///
/// # Safety
/// The pointers `context` holds must remain readable for their accompanying lengths during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_with_syntax(
    context: Option<&ParseContext>,
    source: FfiUtf16View,
    syntax: Option<&SyntaxNode>,
    out_status: Option<&mut FfiParseStatus>,
) -> *const c_void {
    let (Some(context), Some(syntax), Some(out_status)) = (context, syntax, out_status) else {
        return std::ptr::null();
    };
    let Some(source) = source.to_utf16() else {
        *out_status = FfiParseStatus::Invalid;
        return std::ptr::null();
    };
    let Some(parsed) = parse_with_syntax(context, &source, syntax) else {
        *out_status = FfiParseStatus::Invalid;
        return std::ptr::null();
    };
    *out_status = FfiParseStatus::Parsed;
    Arc::into_raw(Arc::new(parsed)).cast()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::parser::value_parser::RandomFunctionIndex;

    fn context(random_function_index: &RandomFunctionIndex) -> ParseContext<'_> {
        ParseContext {
            in_quirks_mode: false,
            is_svg_presentation_attribute: false,
            is_substituted_value: false,
            contains_attr_tainted_values: false,
            is_ua_style_sheet: false,
            value_contexts: ak::Span::default(),
            declared_namespaces: None,
            document_url: ak::Span::default(),
            document_base_url: ak::Span::default(),
            length_resolution_context: None,
            random_function_index: Some(random_function_index),
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
        let random_function_index = RandomFunctionIndex::default();
        let context = context(&random_function_index);
        let syntax = parse_syntax(&utf16("<length> | <number>#"), true).unwrap();
        assert!(parse_with_syntax(&context, &utf16("12px"), &syntax).is_some());
        assert!(parse_with_syntax(&context, &utf16("1, 2, calc(1 + 2)"), &syntax).is_some());
        assert!(parse_with_syntax(&context, &utf16("red"), &syntax).is_none());
        assert!(parse_with_syntax(&context, &utf16("1,"), &syntax).is_none());
    }

    #[test]
    fn universal_syntax_uses_provided_source_text() {
        let random_function_index = RandomFunctionIndex::default();
        let context = context(&random_function_index);
        let source = utf16("1.00px");
        let values = consume_a_list_of_component_values(
            crate::css::css_tokenizer::tokenize_for_parser_without_source(b"1.00px").into_vec(),
        )
        .unwrap();
        let Some(StyleValueData::Unresolved { source_text, .. }) =
            parse_with_syntax_component_values(&context, &values, &SyntaxNode::Universal, Some(&source))
        else {
            panic!("expected an unresolved value");
        };
        assert_eq!(source_text.units(), source);
    }

    #[test]
    fn universal_syntax_serializes_component_values_without_original_source() {
        let random_function_index = RandomFunctionIndex::default();
        let context = context(&random_function_index);
        let mut values = consume_a_list_of_component_values(tokenize_for_parser(b"a b".as_slice())).unwrap();
        values.remove(1);
        let Some(StyleValueData::Unresolved {
            source_text,
            components,
            ..
        }) = parse_with_syntax_component_values(&context, &values, &SyntaxNode::Universal, None)
        else {
            panic!("expected an unresolved value");
        };
        assert_eq!(source_text.units(), utf16("a/**/b"));
        assert_eq!(components.as_slice(), values.as_slice());
    }

    #[test]
    fn serializes_syntax_like_the_cpp_tree() {
        let syntax = parse_syntax(&utf16("<length> | auto | <color>#"), true).unwrap();
        let mut sink = TextSink::new();
        serialize_syntax(&syntax, &mut sink);
        assert_eq!(sink.into_utf16(), utf16("<length> | auto | <color>#"));

        let syntax = parse_syntax(&utf16(r"two\ words"), false).unwrap();
        let mut sink = TextSink::new();
        serialize_syntax(&syntax, &mut sink);
        assert_eq!(sink.into_utf16(), utf16(r"two\ words"));
    }

    #[test]
    fn identifies_single_syntax_components() {
        for source in ["*", "auto", "<length>", "<length>+"] {
            assert!(is_single_syntax_component(&parse_syntax(&utf16(source), true).unwrap()));
        }
        assert!(!is_single_syntax_component(
            &parse_syntax(&utf16("<length> | auto"), true).unwrap()
        ));
    }

    fn utf16(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }
}
