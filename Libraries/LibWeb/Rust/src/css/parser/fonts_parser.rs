/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(clippy::arc_with_non_send_sync)]

use super::component_value::ComponentValue;
use super::token_stream::TokenStream;
use super::value_parser::{
    FontDescriptorKind, NumericRange, ParseContext, ParseOutcome, equals_ascii_case_insensitive, is_valid_custom_ident,
    parse_angle_from_stream, parse_integer_from_stream, parse_number_from_stream, retain_fly_string,
    string_style_value, value_list,
};
use crate::css::css_enums::{
    font_tech, keyword, keyword_from_ascii_case_insensitive, keyword_to_common_lig_value,
    keyword_to_contextual_alt_value, keyword_to_discretionary_lig_value, keyword_to_east_asian_variant,
    keyword_to_east_asian_width, keyword_to_font_style_keyword, keyword_to_font_tech, keyword_to_generic_font_family,
    keyword_to_historical_lig_value, keyword_to_numeric_figure_value, keyword_to_numeric_fraction_value,
    keyword_to_numeric_spacing_value,
};
use crate::css::property_metadata::property_id;
use crate::css::style_value::{
    RetainedByteList, RetainedRequestUrlModifierList, RetainedString, RetainedStyleValueData,
    RetainedStyleValueDataList, StyleValueData,
};
use std::sync::Arc;

fn retained(value: StyleValueData) -> RetainedStyleValueData {
    RetainedStyleValueData::from_owned(value)
}

fn keyword_value(value: &ComponentValue) -> Option<(u16, StyleValueData)> {
    let keyword = keyword_from_ascii_case_insensitive(value.ident()?)?;
    Some((keyword, StyleValueData::Keyword { keyword }))
}

fn parse_family_name(context: &ParseContext, values: &[ComponentValue]) -> Option<StyleValueData> {
    let values = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    if let [value] = values.as_slice()
        && let Some(string) = value.string()
    {
        return string_style_value(context, string);
    }

    let parts = values.iter().map(|value| value.ident()).collect::<Option<Vec<_>>>()?;
    if parts.is_empty() {
        return None;
    }
    if let [part] = parts.as_slice() {
        if !is_valid_custom_ident(part, &[]) {
            return None;
        }
        if keyword_from_ascii_case_insensitive(part)
            .is_some_and(|keyword| keyword_to_generic_font_family(keyword).is_some())
        {
            return None;
        }
    }

    let mut complete_name = Vec::new();
    for (index, part) in parts.iter().enumerate() {
        if index != 0 {
            complete_name.push(u16::from(b' '));
        }
        complete_name.extend_from_slice(part);
    }
    Some(StyleValueData::CustomIdent {
        custom_ident: retain_fly_string(context, &complete_name)?,
    })
}

fn parse_font_family(context: &ParseContext, values: &[ComponentValue]) -> Option<StyleValueData> {
    let families = values
        .split(ComponentValue::is_comma)
        .map(|family| {
            let single = family.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
            if let Some(keyword) = single
                .first()
                .and_then(|value| value.ident())
                .and_then(keyword_from_ascii_case_insensitive)
                .filter(|keyword| keyword_to_generic_font_family(*keyword).is_some())
            {
                return (single.len() == 1).then_some(StyleValueData::Keyword { keyword });
            }
            parse_family_name(context, family)
        })
        .collect::<Option<Vec<_>>>()?;
    (!families.is_empty()).then(|| value_list(families, 1, true))
}

fn parse_font_style(context: &ParseContext, values: &[ComponentValue]) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(values);
    tokens.discard_whitespace();
    let (keyword, _) = keyword_value(tokens.consume_a_token())?;
    let font_style = keyword_to_font_style_keyword(keyword)?;
    tokens.discard_whitespace();
    let angle_value = if keyword == keyword::OBLIQUE && tokens.has_next_token() {
        Some(parse_angle_from_stream(
            context,
            property_id::FONT_STYLE,
            &mut tokens,
            NumericRange::new(-90.0, 90.0),
        )?)
    } else {
        None
    };
    tokens.discard_whitespace();
    if tokens.has_next_token() {
        return None;
    }
    Some(StyleValueData::FontStyle {
        font_style,
        angle_value: angle_value.map_or_else(RetainedStyleValueData::none, retained),
    })
}

fn parse_font_variant_tuple(
    values: &[ComponentValue],
    slots: usize,
    classify: impl Fn(u16) -> Option<usize>,
) -> Option<StyleValueData> {
    let mut tuple = (0..slots).map(|_| RetainedStyleValueData::none()).collect::<Vec<_>>();
    let mut found = false;
    for value in values.iter().filter(|value| !value.is_whitespace()) {
        let (keyword, keyword_value) = keyword_value(value)?;
        let slot = classify(keyword)?;
        if tuple[slot].optional_data().is_some() {
            return None;
        }
        tuple[slot] = retained(keyword_value);
        found = true;
    }
    found.then(|| StyleValueData::Tuple {
        values: RetainedStyleValueDataList::from_retained_values(tuple),
    })
}

fn parse_font_variant_east_asian(values: &[ComponentValue]) -> Option<StyleValueData> {
    parse_font_variant_tuple(values, 3, |keyword| {
        if keyword == keyword::RUBY {
            Some(2)
        } else if keyword_to_east_asian_width(keyword).is_some() {
            Some(1)
        } else if keyword_to_east_asian_variant(keyword).is_some() {
            Some(0)
        } else {
            None
        }
    })
}

fn parse_font_variant_numeric(values: &[ComponentValue]) -> Option<StyleValueData> {
    parse_font_variant_tuple(values, 5, |keyword| {
        if keyword_to_numeric_figure_value(keyword).is_some() {
            Some(0)
        } else if keyword_to_numeric_spacing_value(keyword).is_some() {
            Some(1)
        } else if keyword_to_numeric_fraction_value(keyword).is_some() {
            Some(2)
        } else if keyword == keyword::ORDINAL {
            Some(3)
        } else if keyword == keyword::SLASHED_ZERO {
            Some(4)
        } else {
            None
        }
    })
}

fn parse_font_variant_ligatures(values: &[ComponentValue]) -> Option<StyleValueData> {
    parse_font_variant_tuple(values, 4, |keyword| {
        if keyword_to_common_lig_value(keyword).is_some() {
            Some(0)
        } else if keyword_to_discretionary_lig_value(keyword).is_some() {
            Some(1)
        } else if keyword_to_historical_lig_value(keyword).is_some() {
            Some(2)
        } else if keyword_to_contextual_alt_value(keyword).is_some() {
            Some(3)
        } else {
            None
        }
    })
}

fn parse_feature_value_names(context: &ParseContext, values: &[ComponentValue]) -> Option<Vec<StyleValueData>> {
    values
        .split(ComponentValue::is_comma)
        .map(|argument| {
            let mut argument = argument.iter().filter(|value| !value.is_whitespace());
            let identifier = argument.next()?.ident()?;
            if argument.next().is_some() || !is_valid_custom_ident(identifier, &[]) {
                return None;
            }
            Some(StyleValueData::CustomIdent {
                custom_ident: retain_fly_string(context, identifier)?,
            })
        })
        .collect()
}

fn parse_font_variant_alternates(context: &ParseContext, values: &[ComponentValue]) -> Option<StyleValueData> {
    let mut parsed = [None, None, None, None, None, None, None];
    for value in values.iter().filter(|value| !value.is_whitespace()) {
        if value
            .ident()
            .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"historical-forms"))
        {
            if parsed[1].is_some() {
                return None;
            }
            parsed[1] = Some(StyleValueData::Keyword {
                keyword: keyword::HISTORICAL_FORMS,
            });
            continue;
        }
        let (name, arguments) = value.function()?;
        let (slot, canonical_name, requires_one) = if equals_ascii_case_insensitive(name, b"stylistic") {
            (0, "stylistic", true)
        } else if equals_ascii_case_insensitive(name, b"styleset") {
            (2, "styleset", false)
        } else if equals_ascii_case_insensitive(name, b"character-variant") {
            (3, "character-variant", false)
        } else if equals_ascii_case_insensitive(name, b"swash") {
            (4, "swash", true)
        } else if equals_ascii_case_insensitive(name, b"ornaments") {
            (5, "ornaments", true)
        } else if equals_ascii_case_insensitive(name, b"annotation") {
            (6, "annotation", true)
        } else {
            return None;
        };
        if parsed[slot].is_some() {
            return None;
        }
        let names = parse_feature_value_names(context, arguments)?;
        if names.is_empty() || (requires_one && names.len() != 1) {
            return None;
        }
        let function_name = canonical_name.encode_utf16().collect::<Vec<_>>();
        parsed[slot] = Some(StyleValueData::Function {
            name: retain_fly_string(context, &function_name)?,
            value: retained(value_list(names, 1, true)),
        });
    }
    let parsed = parsed.into_iter().flatten().collect::<Vec<_>>();
    (!parsed.is_empty()).then(|| value_list(parsed, 0, true))
}

fn contains_substitution(values: &[ComponentValue]) -> bool {
    values.iter().any(|value| {
        let Some((name, arguments)) = value.function() else {
            return false;
        };
        (name.len() >= 2 && name[0] == u16::from(b'-') && name[1] == u16::from(b'-'))
            || ["attr", "env", "if", "inherit", "var"]
                .iter()
                .any(|function| equals_ascii_case_insensitive(name, function.as_bytes()))
            || contains_substitution(arguments)
    })
}

fn parse_opentype_tag(context: &ParseContext, tokens: &mut TokenStream<'_>) -> Option<([u16; 4], StyleValueData)> {
    tokens.discard_whitespace();
    let string = tokens.consume_a_token().string()?;
    let tag: [u16; 4] = string.try_into().ok()?;
    if !tag.iter().all(|code_unit| (0x20..=0x7e).contains(code_unit)) {
        return None;
    }
    Some((tag, string_style_value(context, &tag)?))
}

fn open_type_tagged(context: &ParseContext, mode: u8, tag: [u16; 4], value: StyleValueData) -> Option<StyleValueData> {
    let packed_tag = u32::from(tag[0]) << 24 | u32::from(tag[1]) << 16 | u32::from(tag[2]) << 8 | u32::from(tag[3]);
    Some(StyleValueData::OpenTypeTagged {
        mode,
        tag: retain_fly_string(context, &tag)?,
        packed_tag,
        value: retained(value),
    })
}

fn parse_font_feature_settings(context: &ParseContext, values: &[ComponentValue]) -> Option<StyleValueData> {
    let settings = values
        .split(ComponentValue::is_comma)
        .map(|setting| {
            let mut tokens = TokenStream::new(setting);
            let (tag, _) = parse_opentype_tag(context, &mut tokens)?;
            tokens.discard_whitespace();
            let value = if tokens.has_next_token() {
                let saved_position = tokens.position;
                parse_integer_from_stream(
                    context,
                    property_id::FONT_FEATURE_SETTINGS,
                    &mut tokens,
                    NumericRange::NON_NEGATIVE,
                )
                .or_else(|| {
                    tokens.position = saved_position;
                    let identifier = tokens.consume_a_token().ident()?;
                    if equals_ascii_case_insensitive(identifier, b"on") {
                        Some(StyleValueData::Integer { value: 1 })
                    } else if equals_ascii_case_insensitive(identifier, b"off") {
                        Some(StyleValueData::Integer { value: 0 })
                    } else {
                        None
                    }
                })?
            } else {
                StyleValueData::Integer { value: 1 }
            };
            tokens.discard_whitespace();
            if tokens.has_next_token() {
                return None;
            }
            open_type_tagged(context, 0, tag, value)
        })
        .collect::<Option<Vec<_>>>()?;
    (!settings.is_empty()).then(|| value_list(settings, 1, true))
}

fn parse_font_variation_settings(context: &ParseContext, values: &[ComponentValue]) -> Option<StyleValueData> {
    let settings = values
        .split(ComponentValue::is_comma)
        .map(|setting| {
            let mut tokens = TokenStream::new(setting);
            let (tag, _) = parse_opentype_tag(context, &mut tokens)?;
            let value = parse_number_from_stream(
                context,
                property_id::FONT_VARIATION_SETTINGS,
                &mut tokens,
                NumericRange::INFINITE,
            )?;
            tokens.discard_whitespace();
            if tokens.has_next_token() {
                return None;
            }
            open_type_tagged(context, 1, tag, value)
        })
        .collect::<Option<Vec<_>>>()?;
    (!settings.is_empty()).then(|| value_list(settings, 1, true))
}

fn font_format_is_supported(context: &ParseContext, format: &[u16]) -> bool {
    context
        .font_format_is_supported
        .is_some_and(|callback| unsafe { callback(format.as_ptr(), format.len()) })
}

fn parse_font_format(context: &ParseContext, values: &[ComponentValue], tech: &mut Vec<u8>) -> Option<Vec<u16>> {
    let mut values = values.iter().filter(|value| !value.is_whitespace());
    let value = values.next()?;
    if values.next().is_some() {
        return None;
    }
    let format = if let Some(identifier) = value.ident() {
        identifier.to_vec()
    } else {
        let string = value.string()?;
        let (format, variations) = if equals_ascii_case_insensitive(string, b"woff2") {
            ("woff2", false)
        } else if equals_ascii_case_insensitive(string, b"woff") {
            ("woff", false)
        } else if equals_ascii_case_insensitive(string, b"truetype") {
            ("truetype", false)
        } else if equals_ascii_case_insensitive(string, b"opentype") {
            ("opentype", false)
        } else if equals_ascii_case_insensitive(string, b"collection") {
            ("collection", false)
        } else if equals_ascii_case_insensitive(string, b"woff2-variations") {
            ("woff2", true)
        } else if equals_ascii_case_insensitive(string, b"woff-variations") {
            ("woff", true)
        } else if equals_ascii_case_insensitive(string, b"truetype-variations") {
            ("truetype", true)
        } else if equals_ascii_case_insensitive(string, b"opentype-variations") {
            ("opentype", true)
        } else {
            return None;
        };
        if variations {
            tech.push(font_tech::VARIATIONS);
        }
        format.encode_utf16().collect()
    };
    font_format_is_supported(context, &format).then_some(format)
}

fn parse_font_tech(context: &ParseContext, values: &[ComponentValue]) -> Option<Vec<u8>> {
    let callback = context.font_tech_is_supported?;
    values
        .split(ComponentValue::is_comma)
        .map(|item| {
            let mut item = item.iter().filter(|value| !value.is_whitespace());
            let keyword = keyword_from_ascii_case_insensitive(item.next()?.ident()?)?;
            if item.next().is_some() {
                return None;
            }
            let tech = keyword_to_font_tech(keyword)?;
            unsafe { callback(tech) }.then_some(tech)
        })
        .collect()
}

fn parse_local_family_name(context: &ParseContext, values: &[ComponentValue]) -> Option<StyleValueData> {
    let mut end = 0;
    let mut saw_value = false;
    for (index, value) in values.iter().enumerate() {
        if value.is_whitespace() {
            if saw_value {
                end = index + 1;
            }
            continue;
        }
        if !saw_value && value.string().is_some() {
            return parse_family_name(context, &values[..=index]);
        }
        if value.ident().is_none() {
            break;
        }
        saw_value = true;
        end = index + 1;
    }
    parse_family_name(context, &values[..end])
}

fn parse_font_source(context: &ParseContext, values: &[ComponentValue]) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(values);
    tokens.discard_whitespace();
    if let Some((name, arguments)) = tokens.next_token().function()
        && equals_ascii_case_insensitive(name, b"local")
    {
        let local_name = parse_local_family_name(context, arguments)?;
        tokens.discard_a_token();
        tokens.discard_whitespace();
        if tokens.has_next_token() {
            return None;
        }
        return Some(StyleValueData::FontSource {
            is_local: true,
            local_name: retained(local_name),
            url: RetainedString::from_utf8(String::new()),
            url_type: 0,
            url_modifiers: RetainedRequestUrlModifierList::from_retained_modifiers(Vec::new()),
            has_format: false,
            format: crate::css::retained_fly_string::RetainedUtf16FlyString::none(),
            tech: RetainedByteList::from_bytes(Vec::new()),
        });
    }

    let url = super::value_parser::parse_url_value(context, tokens.consume_a_token())?;
    let StyleValueData::Url {
        url,
        url_type,
        modifiers: url_modifiers,
    } = url
    else {
        unreachable!()
    };
    tokens.discard_whitespace();
    let mut tech = Vec::new();
    let mut format = None;
    if let Some((name, arguments)) = tokens.next_token().function()
        && equals_ascii_case_insensitive(name, b"format")
    {
        format = Some(parse_font_format(context, arguments, &mut tech)?);
        tokens.discard_a_token();
        tokens.discard_whitespace();
    }
    if let Some((name, arguments)) = tokens.next_token().function()
        && equals_ascii_case_insensitive(name, b"tech")
    {
        tech.extend(parse_font_tech(context, arguments)?);
        tokens.discard_a_token();
        tokens.discard_whitespace();
    }
    if tokens.has_next_token() {
        return None;
    }
    Some(StyleValueData::FontSource {
        is_local: false,
        local_name: RetainedStyleValueData::none(),
        url,
        url_type,
        url_modifiers,
        has_format: format.is_some(),
        format: match format {
            Some(format) => retain_fly_string(context, &format)?,
            None => crate::css::retained_fly_string::RetainedUtf16FlyString::none(),
        },
        tech: RetainedByteList::from_bytes(tech),
    })
}

fn parse_unicode_range_text(text: &[u16]) -> Option<StyleValueData> {
    let text = String::from_utf16(text).ok()?;
    let text = text.as_bytes();
    if text.len() < 3 || !text[0].eq_ignore_ascii_case(&b'u') || text[1] != b'+' {
        return None;
    }
    let body = &text[2..];
    let (start, end) = if body.contains(&b'?') {
        if body.len() > 6 || body.is_empty() {
            return None;
        }
        let first_question = body.iter().position(|byte| *byte == b'?')?;
        if !body[..first_question].iter().all(u8::is_ascii_hexdigit)
            || !body[first_question..].iter().all(|byte| *byte == b'?')
        {
            return None;
        }
        let start = body
            .iter()
            .map(|byte| if *byte == b'?' { b'0' } else { *byte })
            .collect::<Vec<_>>();
        let end = body
            .iter()
            .map(|byte| if *byte == b'?' { b'F' } else { *byte })
            .collect::<Vec<_>>();
        (
            u32::from_str_radix(std::str::from_utf8(&start).ok()?, 16).ok()?,
            u32::from_str_radix(std::str::from_utf8(&end).ok()?, 16).ok()?,
        )
    } else {
        let (start, end) = match body.iter().position(|byte| *byte == b'-') {
            Some(position) => (&body[..position], &body[position + 1..]),
            None => (body, body),
        };
        if start.is_empty() || start.len() > 6 || end.is_empty() || end.len() > 6 {
            return None;
        }
        (
            u32::from_str_radix(std::str::from_utf8(start).ok()?, 16).ok()?,
            u32::from_str_radix(std::str::from_utf8(end).ok()?, 16).ok()?,
        )
    };
    (start <= end && end <= 0x10ffff).then_some(StyleValueData::UnicodeRange {
        min_code_point: start,
        max_code_point: end,
    })
}

fn parse_unicode_ranges(values: &[ComponentValue]) -> Option<StyleValueData> {
    let ranges = values
        .split(ComponentValue::is_comma)
        .map(|range| {
            let start = range.iter().position(|value| !value.is_whitespace())?;
            let end = range.iter().rposition(|value| !value.is_whitespace())? + 1;
            let range = &range[start..end];
            if range.iter().any(ComponentValue::is_whitespace) {
                return None;
            }
            if range.is_empty() {
                return None;
            }
            let mut text = Vec::new();
            for value in range {
                value.original_source_text.append_to(&mut text);
            }
            parse_unicode_range_text(&text)
        })
        .collect::<Option<Vec<_>>>()?;
    (!ranges.is_empty()).then(|| value_list(ranges, 1, true))
}

pub(crate) fn parse_font_descriptor(
    context: &ParseContext,
    kind: FontDescriptorKind,
    values: &[ComponentValue],
) -> ParseOutcome {
    if contains_substitution(values) {
        return ParseOutcome::NotHandled;
    }
    let parsed = match kind {
        FontDescriptorKind::FamilyName => parse_family_name(context, values),
        FontDescriptorKind::SourceList => {
            let sources = values
                .split(ComponentValue::is_comma)
                .filter_map(|source| parse_font_source(context, source))
                .collect::<Vec<_>>();
            (!sources.is_empty()).then(|| value_list(sources, 1, true))
        }
        FontDescriptorKind::UnicodeRangeList => parse_unicode_ranges(values),
    };
    parsed.map_or(ParseOutcome::Invalid, |parsed| ParseOutcome::Parsed(Arc::new(parsed)))
}

pub(crate) fn parse_font_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if !matches!(
        property,
        property_id::FONT_FAMILY
            | property_id::FONT_STYLE
            | property_id::FONT_FEATURE_SETTINGS
            | property_id::FONT_VARIATION_SETTINGS
            | property_id::FONT_VARIANT_ALTERNATES
            | property_id::FONT_VARIANT_EAST_ASIAN
            | property_id::FONT_VARIANT_LIGATURES
            | property_id::FONT_VARIANT_NUMERIC
    ) {
        return ParseOutcome::NotHandled;
    }
    if contains_substitution(values) {
        return ParseOutcome::NotHandled;
    }
    let single_keyword = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    if let [value] = single_keyword.as_slice()
        && let Some(keyword) = keyword_from_ascii_case_insensitive(value.ident().unwrap_or_default())
        && ((keyword == keyword::NORMAL
            && matches!(
                property,
                property_id::FONT_FEATURE_SETTINGS
                    | property_id::FONT_VARIATION_SETTINGS
                    | property_id::FONT_VARIANT_ALTERNATES
                    | property_id::FONT_VARIANT_EAST_ASIAN
                    | property_id::FONT_VARIANT_LIGATURES
                    | property_id::FONT_VARIANT_NUMERIC
            ))
            || (keyword == keyword::NONE && property == property_id::FONT_VARIANT_LIGATURES))
    {
        return ParseOutcome::Parsed(Arc::new(StyleValueData::Keyword { keyword }));
    }
    let parsed = match property {
        property_id::FONT_FAMILY => parse_font_family(context, values),
        property_id::FONT_STYLE => parse_font_style(context, values),
        property_id::FONT_FEATURE_SETTINGS => parse_font_feature_settings(context, values),
        property_id::FONT_VARIATION_SETTINGS => parse_font_variation_settings(context, values),
        property_id::FONT_VARIANT_ALTERNATES => parse_font_variant_alternates(context, values),
        property_id::FONT_VARIANT_EAST_ASIAN => parse_font_variant_east_asian(values),
        property_id::FONT_VARIANT_LIGATURES => parse_font_variant_ligatures(values),
        property_id::FONT_VARIANT_NUMERIC => parse_font_variant_numeric(values),
        _ => unreachable!(),
    };
    parsed.map_or(ParseOutcome::Invalid, |parsed| ParseOutcome::Parsed(Arc::new(parsed)))
}

#[cfg(test)]
mod tests {
    use super::{FontDescriptorKind, parse_font_descriptor, parse_font_property};
    use crate::css::css_tokenizer::tokenize_for_parser;
    use crate::css::parser::component_value::consume_a_list_of_component_values;
    use crate::css::parser::value_parser::{ParseContext, ParseOutcome};
    use crate::css::property_metadata::property_id;

    unsafe extern "C" fn discard_interned_string(_: *const u16, _: usize) -> usize {
        0
    }

    unsafe extern "C" fn support_font_format(_: *const u16, _: usize) -> bool {
        true
    }

    unsafe extern "C" fn support_font_tech(_: u8) -> bool {
        true
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
            font_format_is_supported: Some(support_font_format),
            font_tech_is_supported: Some(support_font_tech),
            descriptor_integer_resolution_context: std::ptr::null(),
            resolve_descriptor_integer: None,
            random_function_index: std::ptr::null_mut(),
        }
    }

    fn parse(property: u16, source: &str) -> ParseOutcome {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        parse_font_property(&context(), property, &values)
    }

    fn parse_descriptor(kind: FontDescriptorKind, source: &str) -> ParseOutcome {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        parse_font_descriptor(&context(), kind, &values)
    }

    #[test]
    fn parses_font_families() {
        for source in ["serif", "\"Ladybird Sans\"", "Gill Sans, sans-serif", "inherit font"] {
            assert!(
                matches!(parse(property_id::FONT_FAMILY, source), ParseOutcome::Parsed(_)),
                "{source}"
            );
        }
        for source in ["", "my \"font\"", "inherit", "serif sans-serif", "cursive serif"] {
            assert!(
                matches!(parse(property_id::FONT_FAMILY, source), ParseOutcome::Invalid),
                "{source}"
            );
        }
    }

    #[test]
    fn parses_font_style() {
        for source in ["normal", "italic", "left", "right", "oblique", "oblique -20deg"] {
            assert!(
                matches!(parse(property_id::FONT_STYLE, source), ParseOutcome::Parsed(_)),
                "{source}"
            );
        }
        for source in ["oblique 91deg", "italic 10deg", "bogus"] {
            assert!(
                matches!(parse(property_id::FONT_STYLE, source), ParseOutcome::Invalid),
                "{source}"
            );
        }
    }

    #[test]
    fn parses_font_variants() {
        for (property, source) in [
            (
                property_id::FONT_VARIANT_ALTERNATES,
                "historical-forms styleset(foo, bar)",
            ),
            (property_id::FONT_VARIANT_EAST_ASIAN, "jis78 full-width ruby"),
            (property_id::FONT_VARIANT_LIGATURES, "common-ligatures no-contextual"),
            (property_id::FONT_VARIANT_NUMERIC, "oldstyle-nums tabular-nums ordinal"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        for (property, source) in [
            (property_id::FONT_VARIANT_ALTERNATES, "stylistic(foo, bar)"),
            (property_id::FONT_VARIANT_EAST_ASIAN, "jis78 jis90"),
            (property_id::FONT_VARIANT_LIGATURES, "contextual no-contextual"),
            (property_id::FONT_VARIANT_NUMERIC, "lining-nums oldstyle-nums"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn parses_open_type_settings() {
        for source in ["normal", "\"liga\"", "\"kern\" off, \"salt\" 3", "\"abcd\" calc(1 + 2)"] {
            assert!(
                matches!(
                    parse(property_id::FONT_FEATURE_SETTINGS, source),
                    ParseOutcome::Parsed(_)
                ),
                "{source}"
            );
        }
        for source in ["\"wght\" 400", "\"slnt\" -12.5", "\"abcd\" calc(1 + 2)"] {
            assert!(
                matches!(
                    parse(property_id::FONT_VARIATION_SETTINGS, source),
                    ParseOutcome::Parsed(_)
                ),
                "{source}"
            );
        }
        for (property, source) in [
            (property_id::FONT_FEATURE_SETTINGS, "\"abc\""),
            (property_id::FONT_FEATURE_SETTINGS, "\"liga\" -1"),
            (property_id::FONT_VARIATION_SETTINGS, "\"wght\""),
            (property_id::FONT_VARIATION_SETTINGS, "\"wide!\" 1"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn parses_font_descriptors() {
        for source in [
            "local(Ladybird Sans)",
            "url(font.woff2) format(woff2)",
            "url(font.woff2) format(\"woff2-variations\") tech(color-COLRv1)",
            "local(serif 1), url(font.woff2)",
        ] {
            assert!(
                matches!(
                    parse_descriptor(FontDescriptorKind::SourceList, source),
                    ParseOutcome::Parsed(_)
                ),
                "{source}"
            );
        }
        for source in ["U+26", "U+0-10FFFF", "U+4??, U+1F600-1F64F"] {
            assert!(
                matches!(
                    parse_descriptor(FontDescriptorKind::UnicodeRangeList, source),
                    ParseOutcome::Parsed(_)
                ),
                "{source}"
            );
        }
        assert!(matches!(
            parse_descriptor(FontDescriptorKind::FamilyName, "Ladybird Sans"),
            ParseOutcome::Parsed(_)
        ));
        for source in ["U +26", "U+110000", "U+20-10"] {
            assert!(matches!(
                parse_descriptor(FontDescriptorKind::UnicodeRangeList, source),
                ParseOutcome::Invalid
            ));
        }
    }
}
