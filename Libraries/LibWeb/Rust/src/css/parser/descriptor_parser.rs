/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::arc_with_non_send_sync)]

use crate::css::css_enums::{
    keyword, keyword_from_ascii_case_insensitive, keyword_to_counter_style_name_keyword,
    keyword_to_counter_style_system, keyword_to_page_size,
};
use crate::css::css_tokenizer::tokenize_for_parser;
use crate::css::descriptor_metadata::{DescriptorMetadata, DescriptorSyntax, DescriptorValueType, descriptor_metadata};
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::arbitrary_substitution::{
    SubstitutionFunctionsPresence, collect_arbitrary_substitution_function_presence, declaration_value_is_valid,
};
use crate::css::parser::component_value::{ComponentValue, consume_a_list_of_component_values};
use crate::css::parser::fonts_parser::parse_font_descriptor;
use crate::css::parser::token_stream::TokenStream;
use crate::css::parser::value_parser::{
    FfiValueParsingContext, FfiValueParsingContextKind, FontDescriptorKind, NumericRange, ParseContext, ParseOutcome,
    is_valid_custom_ident, parse_css_value, parse_integer_from_stream, parse_length_value, parse_percentage_value,
    retain_fly_string, string_style_value, unresolved_value, value_list,
};
use crate::css::property_metadata::property_id;
use crate::css::style_value::{RetainedStyleValueData, RetainedUtf16FlyString, StyleValueData};
use std::ffi::c_void;
use std::sync::Arc;

pub(crate) struct ParsedDescriptor {
    pub value: Arc<StyleValueData>,
}

fn non_whitespace(values: &[ComponentValue]) -> Vec<&ComponentValue> {
    values.iter().filter(|value| !value.is_whitespace()).collect()
}

fn single_non_whitespace(values: &[ComponentValue]) -> Option<&ComponentValue> {
    let values = non_whitespace(values);
    if values.len() != 1 {
        return None;
    }
    Some(values[0])
}

fn is_css_wide_keyword(value: &StyleValueData) -> bool {
    matches!(
        value,
        StyleValueData::Keyword {
            keyword: keyword::INHERIT | keyword::INITIAL | keyword::UNSET | keyword::REVERT | keyword::REVERT_LAYER
        }
    )
}

fn into_owned(value: Arc<StyleValueData>) -> Option<StyleValueData> {
    if let StyleValueData::Keyword { keyword } = &*value {
        return Some(StyleValueData::Keyword { keyword: *keyword });
    }
    Arc::into_inner(value)
}

fn parse_keyword(values: &[ComponentValue], expected: &str) -> Option<StyleValueData> {
    let identifier = single_non_whitespace(values)?.ident()?;
    let parsed = keyword_from_ascii_case_insensitive(identifier)?;
    let expected = keyword_from_ascii_case_insensitive(&expected.encode_utf16().collect::<Vec<_>>())?;
    (parsed == expected).then_some(StyleValueData::Keyword { keyword: parsed })
}

fn parse_counter_style_name(context: &ParseContext, value: &ComponentValue) -> Option<StyleValueData> {
    let identifier = value.ident()?;
    if !is_valid_custom_ident(identifier, &["none"]) {
        return None;
    }
    let name = if keyword_from_ascii_case_insensitive(identifier)
        .is_some_and(|keyword| keyword_to_counter_style_name_keyword(keyword).is_some())
    {
        identifier
            .iter()
            .map(|unit| u16::from(u8::try_from(*unit).unwrap().to_ascii_lowercase()))
            .collect::<Vec<_>>()
    } else {
        identifier.to_vec()
    };
    Some(StyleValueData::CustomIdent {
        custom_ident: retain_fly_string(context, &name)?,
    })
}

fn parse_symbol(context: &ParseContext, value: &ComponentValue) -> Option<StyleValueData> {
    if let Some(string) = value.string() {
        return string_style_value(context, string);
    }
    let identifier = value.ident()?;
    if !is_valid_custom_ident(identifier, &[]) {
        return None;
    }
    Some(StyleValueData::CustomIdent {
        custom_ident: retain_fly_string(context, identifier)?,
    })
}

fn parse_integer_component(
    context: &ParseContext,
    value: &ComponentValue,
    accepted_range: NumericRange,
) -> Option<(i32, StyleValueData)> {
    let mut stream = TokenStream::new(std::slice::from_ref(value));
    let parsed = parse_integer_from_stream(context, property_id::CUSTOM, &mut stream, accepted_range)?;
    stream.discard_whitespace();
    if !stream.is_empty() {
        return None;
    }
    let resolved = match &parsed {
        StyleValueData::Integer { value } => *value,
        StyleValueData::Calculated { .. } => {
            let callback = context.resolve_descriptor_integer?;
            let parsed = Arc::new(parsed);
            let mut resolved = 0;
            if !(unsafe {
                callback(
                    context.descriptor_integer_resolution_context,
                    Arc::as_ptr(&parsed).cast::<c_void>(),
                    &raw mut resolved,
                )
            }) {
                return None;
            }
            return Some((resolved, Arc::into_inner(parsed)?));
        }
        _ => return None,
    };
    Some((resolved, parsed))
}

fn parse_integer_symbol_pair(context: &ParseContext, values: &[ComponentValue]) -> Option<(i32, StyleValueData)> {
    let values = non_whitespace(values);
    if values.len() != 2 {
        return None;
    }
    for (integer_index, symbol_index) in [(0, 1), (1, 0)] {
        if let Some((value, integer)) =
            parse_integer_component(context, values[integer_index], NumericRange::new(0.0, f64::INFINITY))
            && let Some(symbol) = parse_symbol(context, values[symbol_index])
        {
            return Some((value, value_list(vec![integer, symbol], 0, false)));
        }
    }
    None
}

fn parse_counter_style_system(context: &ParseContext, values: &[ComponentValue]) -> Option<StyleValueData> {
    let mut stream = TokenStream::new(values);
    stream.discard_whitespace();
    let identifier = stream.consume_a_token().ident()?;
    let parsed_keyword = keyword_from_ascii_case_insensitive(identifier)?;
    if let Some(system) = keyword_to_counter_style_system(parsed_keyword) {
        stream.discard_whitespace();
        return stream.is_empty().then_some(StyleValueData::CounterStyleSystem {
            kind: 0,
            system,
            first_symbol: RetainedStyleValueData::none(),
            name: RetainedUtf16FlyString::none(),
        });
    }
    if parsed_keyword == keyword::FIXED {
        stream.discard_whitespace();
        let first_symbol = if stream.is_empty() {
            RetainedStyleValueData::none()
        } else {
            RetainedStyleValueData::from_owned(parse_integer_from_stream(
                context,
                property_id::CUSTOM,
                &mut stream,
                NumericRange::new(f64::NEG_INFINITY, f64::INFINITY),
            )?)
        };
        stream.discard_whitespace();
        return stream.is_empty().then_some(StyleValueData::CounterStyleSystem {
            kind: 1,
            system: 0,
            first_symbol,
            name: RetainedUtf16FlyString::none(),
        });
    }
    if parsed_keyword == keyword::EXTENDS {
        stream.discard_whitespace();
        let StyleValueData::CustomIdent { custom_ident } = parse_counter_style_name(context, stream.consume_a_token())?
        else {
            unreachable!();
        };
        stream.discard_whitespace();
        return stream.is_empty().then_some(StyleValueData::CounterStyleSystem {
            kind: 2,
            system: 0,
            first_symbol: RetainedStyleValueData::none(),
            name: custom_ident,
        });
    }
    None
}

fn parse_page_size(context: &ParseContext, values: &[ComponentValue]) -> Option<StyleValueData> {
    if let Some(auto) = parse_keyword(values, "auto") {
        return Some(auto);
    }
    let values = non_whitespace(values);
    if (1..=2).contains(&values.len()) {
        let lengths = values
            .iter()
            .map(|value| {
                parse_length_value(
                    context,
                    property_id::CUSTOM,
                    value,
                    NumericRange::new(0.0, f64::INFINITY),
                )
            })
            .collect::<Option<Vec<_>>>();
        if let Some(mut lengths) = lengths {
            return if lengths.len() == 1 {
                lengths.pop()
            } else {
                Some(value_list(lengths, 0, true))
            };
        }
    }
    let keywords = values
        .iter()
        .map(|value| keyword_from_ascii_case_insensitive(value.ident()?))
        .collect::<Option<Vec<_>>>()?;
    if !(1..=2).contains(&keywords.len()) {
        return None;
    }
    let mut page_size = None;
    let mut orientation = None;
    for keyword in keywords {
        if matches!(keyword, keyword::LANDSCAPE | keyword::PORTRAIT) && orientation.is_none() {
            orientation = Some(keyword);
        } else if keyword_to_page_size(keyword).is_some() && page_size.is_none() {
            page_size = Some(keyword);
        } else {
            return None;
        }
    }
    if page_size.is_some() && orientation == Some(keyword::PORTRAIT) {
        orientation = None;
    }
    let mut result = Vec::new();
    if let Some(keyword) = page_size {
        result.push(StyleValueData::Keyword { keyword });
    }
    if let Some(keyword) = orientation {
        result.push(StyleValueData::Keyword { keyword });
    }
    match result.len() {
        1 => result.pop(),
        2 => Some(value_list(result, 0, true)),
        _ => None,
    }
}

fn parse_counter_style_range(context: &ParseContext, values: &[ComponentValue]) -> Option<StyleValueData> {
    if let Some(auto) = parse_keyword(values, "auto") {
        return Some(auto);
    }
    let mut ranges = Vec::new();
    for range in values.split(ComponentValue::is_comma) {
        let values = non_whitespace(range);
        if values.len() != 2 {
            return None;
        }
        let parse_bound = |value: &ComponentValue, lower: bool| -> Option<(i32, StyleValueData)> {
            if value
                .ident()
                .is_some_and(|ident| keyword_from_ascii_case_insensitive(ident) == Some(keyword::INFINITE))
            {
                return Some((
                    if lower { i32::MIN } else { i32::MAX },
                    StyleValueData::Keyword {
                        keyword: keyword::INFINITE,
                    },
                ));
            }
            parse_integer_component(context, value, NumericRange::new(f64::NEG_INFINITY, f64::INFINITY))
        };
        let (lower, lower_value) = parse_bound(values[0], true)?;
        let (upper, upper_value) = parse_bound(values[1], false)?;
        if lower > upper {
            return None;
        }
        ranges.push(value_list(vec![lower_value, upper_value], 0, false));
    }
    (!ranges.is_empty()).then(|| value_list(ranges, 1, true))
}

fn parse_value_type(
    context: &ParseContext,
    value_type: DescriptorValueType,
    values: &[ComponentValue],
    source: &[u16],
) -> Option<StyleValueData> {
    match value_type {
        DescriptorValueType::FamilyName => {
            match parse_font_descriptor(context, FontDescriptorKind::FamilyName, values) {
                ParseOutcome::Parsed(value) => into_owned(value),
                _ => None,
            }
        }
        DescriptorValueType::FontSrcList => {
            match parse_font_descriptor(context, FontDescriptorKind::SourceList, values) {
                ParseOutcome::Parsed(value) => into_owned(value),
                _ => None,
            }
        }
        DescriptorValueType::UnicodeRangeTokens => {
            match parse_font_descriptor(context, FontDescriptorKind::UnicodeRangeList, values) {
                ParseOutcome::Parsed(value) => into_owned(value),
                _ => None,
            }
        }
        DescriptorValueType::Length => parse_length_value(
            context,
            property_id::CUSTOM,
            single_non_whitespace(values)?,
            NumericRange::new(f64::NEG_INFINITY, f64::INFINITY),
        ),
        DescriptorValueType::PositivePercentage => {
            parse_percentage_value(single_non_whitespace(values)?, NumericRange::new(0.0, f64::INFINITY))
        }
        DescriptorValueType::String => string_style_value(context, single_non_whitespace(values)?.string()?),
        DescriptorValueType::Symbol => parse_symbol(context, single_non_whitespace(values)?),
        DescriptorValueType::Symbols => {
            let symbols = non_whitespace(values)
                .into_iter()
                .map(|value| parse_symbol(context, value))
                .collect::<Option<Vec<_>>>()?;
            (!symbols.is_empty()).then(|| value_list(symbols, 0, false))
        }
        DescriptorValueType::CounterStyleName => parse_counter_style_name(context, single_non_whitespace(values)?),
        DescriptorValueType::CounterStyleNegative => {
            let values = non_whitespace(values);
            if !(1..=2).contains(&values.len()) {
                return None;
            }
            let collapsible = values.len() == 1;
            Some(value_list(
                values
                    .into_iter()
                    .map(|value| parse_symbol(context, value))
                    .collect::<Option<Vec<_>>>()?,
                0,
                collapsible,
            ))
        }
        DescriptorValueType::CounterStylePad => {
            let (_, pair) = parse_integer_symbol_pair(context, values)?;
            Some(pair)
        }
        DescriptorValueType::CounterStyleAdditiveSymbols => {
            let mut previous = i32::MAX;
            let mut tuples = Vec::new();
            for tuple in values.split(ComponentValue::is_comma) {
                let (integer, pair) = parse_integer_symbol_pair(context, tuple)?;
                if integer >= previous {
                    return None;
                }
                previous = integer;
                tuples.push(pair);
            }
            (!tuples.is_empty()).then(|| value_list(tuples, 1, true))
        }
        DescriptorValueType::CounterStyleSystem => parse_counter_style_system(context, values),
        DescriptorValueType::CounterStyleRange => parse_counter_style_range(context, values),
        DescriptorValueType::CropOrCross => {
            let values = non_whitespace(values);
            if !(1..=2).contains(&values.len()) {
                return None;
            }
            let keywords = values
                .into_iter()
                .map(|value| keyword_from_ascii_case_insensitive(value.ident()?))
                .collect::<Option<Vec<_>>>()?;
            if keywords
                .iter()
                .any(|value| !matches!(*value, keyword::CROP | keyword::CROSS))
                || keywords.len() == 2 && keywords[0] == keywords[1]
            {
                return None;
            }
            if keywords.len() == 1 {
                Some(StyleValueData::Keyword { keyword: keywords[0] })
            } else {
                Some(value_list(
                    vec![
                        StyleValueData::Keyword { keyword: keyword::CROP },
                        StyleValueData::Keyword {
                            keyword: keyword::CROSS,
                        },
                    ],
                    0,
                    true,
                ))
            }
        }
        DescriptorValueType::FontWeightAbsolutePair => {
            let values = non_whitespace(values);
            if !(1..=2).contains(&values.len()) {
                return None;
            }
            let parsed = values
                .iter()
                .map(
                    |value| match parse_css_value(context, property_id::FONT_WEIGHT, std::slice::from_ref(value)) {
                        ParseOutcome::Parsed(value)
                            if !is_css_wide_keyword(&value)
                                && !matches!(
                                    &*value,
                                    StyleValueData::Keyword {
                                        keyword: keyword::BOLDER | keyword::LIGHTER
                                    }
                                ) =>
                        {
                            into_owned(value)
                        }
                        _ => None,
                    },
                )
                .collect::<Option<Vec<_>>>()?;
            Some(value_list(parsed, 0, false))
        }
        DescriptorValueType::OptionalDeclarationValue => {
            if !declaration_value_is_valid(values) {
                return None;
            }
            Some(unresolved_value(source, &[], SubstitutionFunctionsPresence::default()))
        }
        DescriptorValueType::PageSize => parse_page_size(context, values),
    }
}

fn parse_with_metadata(
    context: &ParseContext,
    metadata: &DescriptorMetadata,
    values: &[ComponentValue],
    source: &[u16],
) -> Option<StyleValueData> {
    for syntax in metadata.syntax {
        let parsed = match syntax {
            DescriptorSyntax::Keyword(keyword) => parse_keyword(values, keyword),
            DescriptorSyntax::Property(property) => match parse_css_value(context, *property, values) {
                ParseOutcome::Parsed(value) if metadata.allow_css_wide_keywords || !is_css_wide_keyword(&value) => {
                    into_owned(value)
                }
                _ => None,
            },
            DescriptorSyntax::ValueType(value_type) => parse_value_type(context, *value_type, values, source),
        };
        if parsed.is_some() {
            return parsed;
        }
    }
    None
}

pub(crate) fn parse_descriptor(
    context: &ParseContext,
    at_rule: u8,
    name: &[u16],
    values: &[ComponentValue],
    source: &[u16],
) -> Option<ParsedDescriptor> {
    let metadata = descriptor_metadata(at_rule, name)?;
    let inherited_contexts = if context.value_context_count == 0 {
        &[]
    } else if context.value_contexts.is_null() {
        return None;
    } else {
        unsafe { std::slice::from_raw_parts(context.value_contexts, context.value_context_count) }
    };
    let mut value_contexts = Vec::with_capacity(inherited_contexts.len() + 1);
    value_contexts.extend_from_slice(inherited_contexts);
    value_contexts.push(FfiValueParsingContext {
        kind: FfiValueParsingContextKind::Descriptor,
        value: u16::from(at_rule),
        secondary_value: 0,
        name: FfiUtf16View::default(),
    });
    let mut descriptor_context = *context;
    descriptor_context.value_contexts = value_contexts.as_ptr();
    descriptor_context.value_context_count = value_contexts.len();
    let mut presence = SubstitutionFunctionsPresence::default();
    collect_arbitrary_substitution_function_presence(values, &mut presence).ok()?;
    let value = if presence.has_any() {
        if !metadata.allow_arbitrary_substitution_functions {
            return None;
        }
        unresolved_value(source, &[], presence)
    } else {
        parse_with_metadata(&descriptor_context, &metadata, values, source)?
    };
    Some(ParsedDescriptor { value: Arc::new(value) })
}

/// Parses one descriptor value and returns retained Rust style-value data.
///
/// # Safety
/// Every view and `context` must remain readable for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_css_descriptor(
    context: *const ParseContext,
    at_rule: u8,
    name: FfiUtf16View,
    source: FfiUtf16View,
) -> *const c_void {
    crate::abort_on_panic(|| {
        let Some(context) = (unsafe { context.as_ref() }) else {
            return std::ptr::null();
        };
        let Some(name) = (unsafe { name.units() }) else {
            return std::ptr::null();
        };
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null();
        };
        let Ok(values) = consume_a_list_of_component_values(tokenize_for_parser(source)) else {
            return std::ptr::null();
        };
        let mut name_units = Vec::with_capacity(name.len());
        name.append_to(&mut name_units);
        let mut source_units = Vec::with_capacity(source.len());
        source.append_to(&mut source_units);
        parse_descriptor(context, at_rule, &name_units, &values, &source_units)
            .map(|descriptor| Arc::into_raw(descriptor.value).cast::<c_void>())
            .unwrap_or(std::ptr::null())
    })
}

#[cfg(test)]
mod tests {
    use super::{consume_a_list_of_component_values, parse_descriptor, tokenize_for_parser};
    use crate::css::parser::value_parser::ParseContext;
    use crate::css::style_value::StyleValueData;
    use std::sync::Arc;

    unsafe extern "C" fn discard_interned_string(_: *const u16, _: usize) -> usize {
        0
    }

    fn context() -> ParseContext {
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

    fn parse(at_rule: u8, name: &str, source: &str) -> Option<StyleValueData> {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        let source = source.encode_utf16().collect::<Vec<_>>();
        Arc::into_inner(
            parse_descriptor(
                &context(),
                at_rule,
                &name.encode_utf16().collect::<Vec<_>>(),
                &values,
                &source,
            )?
            .value,
        )
    }

    #[test]
    fn parses_representative_descriptors() {
        assert!(matches!(
            parse(0, "font-display", "swap"),
            Some(StyleValueData::Keyword { .. })
        ));
        assert!(matches!(
            parse(0, "font-weight", "bold"),
            Some(StyleValueData::ValueList { .. })
        ));
        assert!(matches!(
            parse(0, "font-width", "condensed"),
            Some(StyleValueData::Keyword { .. })
        ));
        assert!(matches!(
            parse(1, "marks", "cross crop"),
            Some(StyleValueData::ValueList { .. })
        ));
        assert!(matches!(
            parse(1, "size", "portrait"),
            Some(StyleValueData::Keyword { .. })
        ));
        assert!(matches!(
            parse(2, "inherits", "false"),
            Some(StyleValueData::Keyword { .. })
        ));
        assert!(matches!(
            parse(3, "pad", "2 '0'"),
            Some(StyleValueData::ValueList { .. })
        ));
        assert!(matches!(
            parse(3, "symbols", "'X' 'Y'"),
            Some(StyleValueData::ValueList { .. })
        ));
        assert!(matches!(
            parse(3, "negative", "'X' 'Y'"),
            Some(StyleValueData::ValueList { .. })
        ));
        assert!(parse(3, "additive-symbols", "1 a, 2 b").is_none());
        assert!(parse(0, "font-display", "var(--display)").is_none());
        assert!(parse(0, "font-weight", "calc(max(0 * sibling-index(), 400))").is_none());
    }
}
