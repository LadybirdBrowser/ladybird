/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS image and gradient parsing, mirroring ValueParsing.cpp and GradientParsing.cpp.

#![allow(clippy::arc_with_non_send_sync)]

use std::sync::Arc;

use crate::css::css_tokenizer::ParserTokenKind;
use crate::css::parser::color_parser::{color_syntax, parse_color_interpolation_method, parse_color_value};
use crate::css::parser::component_value::{ComponentKind, ComponentValue};
use crate::css::parser::positions_shapes_parser::{center_position, parse_position_from_stream, parse_radial_size};
use crate::css::parser::token_stream::TokenStream;
use crate::css::property_metadata::property_id;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style_value::{
    ImageResourceContext, RetainedColorStop, RetainedColorStopList, RetainedImageSetOption, RetainedImageSetOptionList,
    RetainedRequestUrlModifierList, RetainedString, RetainedStyleValueData, RetainedStyleValueDataList, StyleValueData,
};

use super::value_parser::{
    NumericRange, PROPERTY_NOT_PORTED, ParseContext, ParseOutcome, equals_ascii_case_insensitive,
    parse_angle_from_stream, parse_angle_percentage_from_stream, parse_length_percentage_from_stream,
    parse_resolution_from_stream, parse_url_value, retain_fly_string,
};

fn retained(value: StyleValueData) -> RetainedStyleValueData {
    RetainedStyleValueData::from_owned(value)
}

fn value_list(values: Vec<StyleValueData>) -> StyleValueData {
    StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(values.into_iter().map(retained).collect()),
        separator: 1,
        collapsible: true,
    }
}

fn context_bytes(pointer: *const u8, length: usize) -> Option<&'static [u8]> {
    if pointer.is_null() {
        return (length == 0).then_some(&[]);
    }
    Some(unsafe { std::slice::from_raw_parts(pointer, length) })
}

fn image_resource_context(context: &ParseContext) -> Option<ImageResourceContext> {
    let base_url = context_bytes(context.document_base_url, context.document_base_url_length)?;
    Some(ImageResourceContext {
        base_url: RetainedString::from_utf8(std::str::from_utf8(base_url).ok()?.to_owned()),
        has_base_url: !base_url.is_empty(),
        has_parent_style_sheet_origin_clean: false,
        parent_style_sheet_origin_clean: false,
        should_absolutize_url_for_computed_value: false,
    })
}

fn image_from_url(context: &ParseContext, url: StyleValueData) -> Option<StyleValueData> {
    let StyleValueData::Url {
        url,
        url_type,
        modifiers,
    } = url
    else {
        return None;
    };
    Some(StyleValueData::Image {
        url,
        url_type,
        url_modifiers: modifiers,
        resource_context: image_resource_context(context)?,
    })
}

fn image_from_string(context: &ParseContext, string: &[u16]) -> Option<StyleValueData> {
    Some(StyleValueData::Image {
        url: RetainedString::from_utf16(string)?,
        url_type: 0,
        url_modifiers: RetainedRequestUrlModifierList::from_retained_modifiers(Vec::new()),
        resource_context: image_resource_context(context)?,
    })
}

fn parse_type(context: &ParseContext, value: &ComponentValue) -> Option<RetainedUtf16FlyString> {
    let (name, arguments) = value.function()?;
    if !equals_ascii_case_insensitive(name, b"type") {
        return None;
    }
    let mut arguments = arguments.iter().filter(|value| !value.is_whitespace());
    let string = arguments.next()?.string()?;
    if arguments.next().is_some() {
        return None;
    }
    retain_fly_string(context, string)
}

fn parse_image_set(context: &ParseContext, property: u16, arguments: &[ComponentValue]) -> Option<StyleValueData> {
    // NB: Mirrors the C++ image-set parser's rejection of attr()-tainted options.
    if context.contains_attr_tainted_values {
        return None;
    }
    let mut options = Vec::new();
    for option in arguments.split(ComponentValue::is_comma) {
        let mut tokens = TokenStream::new(option);
        tokens.discard_whitespace();
        let image = if let Some(string) = tokens.next_token().string() {
            let image = image_from_string(context, string)?;
            tokens.discard_a_token();
            image
        } else {
            parse_image_value(context, property, &mut tokens, false)?
        };

        let mut resolution = None;
        let mut type_string = None;
        loop {
            tokens.discard_whitespace();
            if !tokens.has_next_token() {
                break;
            }
            let mut resolution_tokens = tokens.clone();
            if resolution.is_none()
                && let Some(parsed) =
                    parse_resolution_from_stream(context, property, &mut resolution_tokens, NumericRange::INFINITE)
            {
                tokens = resolution_tokens;
                resolution = Some(parsed);
                continue;
            }
            if type_string.is_none()
                && let Some(parsed) = parse_type(context, tokens.next_token())
            {
                tokens.discard_a_token();
                type_string = Some(parsed);
                continue;
            }
            return None;
        }
        options.push(RetainedImageSetOption::from_retained_values(
            retained(image),
            retained(resolution.unwrap_or(StyleValueData::Resolution { value: 1.0, unit: 3 })),
            type_string,
        ));
    }
    (!options.is_empty()).then(|| StyleValueData::ImageSet {
        options: RetainedImageSetOptionList::from_retained_elements(options),
    })
}

fn optional_retained(value: Option<StyleValueData>) -> RetainedStyleValueData {
    value.map_or_else(RetainedStyleValueData::none, retained)
}

struct ParsedColorStop {
    color: StyleValueData,
    position: Option<StyleValueData>,
    second_position: Option<StyleValueData>,
}

enum ColorStopElement {
    Stop(Box<ParsedColorStop>),
    Hint(StyleValueData),
}

fn parse_color_stop_element(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    angular: bool,
) -> Option<ColorStopElement> {
    tokens.discard_whitespace();
    if !tokens.has_next_token() {
        return None;
    }
    let parse_position = |tokens: &mut TokenStream<'_>| {
        if angular {
            if matches!(tokens.next_token().kind, ComponentKind::Token(ParserTokenKind::Number { value, .. }) if value == 0.0)
            {
                tokens.discard_a_token();
                return Some(StyleValueData::Angle { value: 0.0, unit: 0 });
            }
            parse_angle_percentage_from_stream(
                context,
                property,
                tokens,
                NumericRange::INFINITE,
                NumericRange::INFINITE,
            )
        } else {
            parse_length_percentage_from_stream(
                context,
                property,
                tokens,
                NumericRange::INFINITE,
                NumericRange::INFINITE,
            )
        }
    };

    let mut position_tokens = tokens.clone();
    if let Some(position) = parse_position(&mut position_tokens) {
        *tokens = position_tokens;
        tokens.discard_whitespace();
        if !tokens.has_next_token() || tokens.next_token().is_comma() {
            return Some(ColorStopElement::Hint(position));
        }
        let color = parse_color_value(context, property, tokens, false)?;
        return Some(ColorStopElement::Stop(Box::new(ParsedColorStop {
            color,
            position: Some(position),
            second_position: None,
        })));
    }

    let color = parse_color_value(context, property, tokens, false)?;
    tokens.discard_whitespace();
    let mut positions = Vec::new();
    for _ in 0..2 {
        if !tokens.has_next_token() || tokens.next_token().is_comma() {
            break;
        }
        positions.push(parse_position(tokens)?);
        tokens.discard_whitespace();
    }
    Some(ColorStopElement::Stop(Box::new(ParsedColorStop {
        color,
        position: positions.first().cloned(),
        second_position: positions.get(1).cloned(),
    })))
}

fn retained_color_stop(hint: Option<StyleValueData>, stop: ColorStopElement) -> Option<RetainedColorStop> {
    let ColorStopElement::Stop(stop) = stop else {
        return None;
    };
    let ParsedColorStop {
        color,
        position,
        second_position,
    } = *stop;
    Some(RetainedColorStop::from_retained_values(
        optional_retained(hint),
        retained(color),
        optional_retained(position),
        optional_retained(second_position),
    ))
}

fn parse_color_stop_list(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    angular: bool,
) -> Option<(RetainedColorStopList, u8)> {
    let first = parse_color_stop_element(context, property, tokens, angular)?;
    let mut color_syntax_value = match &first {
        ColorStopElement::Stop(stop) => color_syntax(&stop.color),
        ColorStopElement::Hint(_) => return None,
    };
    let mut stops = vec![retained_color_stop(None, first)?];
    loop {
        tokens.discard_whitespace();
        if !tokens.has_next_token() {
            break;
        }
        if !tokens.next_token().is_comma() {
            return None;
        }
        tokens.discard_a_token();
        let element = parse_color_stop_element(context, property, tokens, angular)?;
        let (hint, stop) = match element {
            ColorStopElement::Hint(hint) => {
                tokens.discard_whitespace();
                if !tokens.next_token().is_comma() {
                    return None;
                }
                tokens.discard_a_token();
                (
                    Some(hint),
                    parse_color_stop_element(context, property, tokens, angular)?,
                )
            }
            stop => (None, stop),
        };
        if let ColorStopElement::Stop(stop) = &stop {
            color_syntax_value = color_syntax_value.max(color_syntax(&stop.color));
        }
        stops.push(retained_color_stop(hint, stop)?);
    }
    Some((RetainedColorStopList::from_retained_elements(stops), color_syntax_value))
}

fn strip_ascii_prefix<'a>(value: &'a [u16], prefix: &[u8]) -> Option<&'a [u16]> {
    if value.len() < prefix.len() || !equals_ascii_case_insensitive(&value[..prefix.len()], prefix) {
        return None;
    }
    Some(&value[prefix.len()..])
}

fn side(identifier: &[u16]) -> Option<u8> {
    if equals_ascii_case_insensitive(identifier, b"top") {
        Some(0)
    } else if equals_ascii_case_insensitive(identifier, b"bottom") {
        Some(1)
    } else if equals_ascii_case_insensitive(identifier, b"left") {
        Some(2)
    } else if equals_ascii_case_insensitive(identifier, b"right") {
        Some(3)
    } else {
        None
    }
}

fn corner(first: u8, second: u8) -> Option<u8> {
    match (first, second) {
        (0, 2) | (2, 0) => Some(4),
        (0, 3) | (3, 0) => Some(5),
        (1, 2) | (2, 1) => Some(6),
        (1, 3) | (3, 1) => Some(7),
        _ => None,
    }
}

// https://drafts.csswg.org/css-images-4/#linear-gradients
// <linear-gradient-syntax> = [ [ <angle> | <zero> | to <side-or-corner> ] || <color-interpolation-method> ]? , <color-stop-list>
fn parse_linear_gradient(
    context: &ParseContext,
    property: u16,
    mut name: &[u16],
    arguments: &[ComponentValue],
) -> Option<StyleValueData> {
    let webkit = strip_ascii_prefix(name, b"-webkit-");
    if let Some(stripped) = webkit {
        name = stripped;
    }
    let repeating = strip_ascii_prefix(name, b"repeating-");
    if let Some(stripped) = repeating {
        name = stripped;
    }
    if !equals_ascii_case_insensitive(name, b"linear-gradient") {
        return None;
    }

    let mut tokens = TokenStream::new(arguments);
    tokens.discard_whitespace();
    if !tokens.has_next_token() {
        return None;
    }
    let mut interpolation_method = parse_color_interpolation_method(&mut tokens);
    tokens.discard_whitespace();
    let mut direction_value = None;
    let mut side_or_corner = if webkit.is_some() { 0 } else { 1 };
    let mut has_direction = true;
    let mut angle_tokens = tokens.clone();
    if let Some(angle) = parse_angle_from_stream(context, property, &mut angle_tokens, NumericRange::INFINITE) {
        tokens = angle_tokens;
        direction_value = Some(angle);
    } else if matches!(tokens.next_token().kind, ComponentKind::Token(ParserTokenKind::Number { value, .. }) if value == 0.0)
    {
        tokens.discard_a_token();
        direction_value = Some(StyleValueData::Angle { value: 0.0, unit: 0 });
    } else {
        let is_side = tokens.next_token().ident().is_some_and(|identifier| {
            if webkit.is_some() {
                side(identifier).is_some()
            } else {
                equals_ascii_case_insensitive(identifier, b"to")
            }
        });
        if is_side {
            if webkit.is_none() {
                tokens.discard_a_token();
                tokens.discard_whitespace();
            }
            let first = side(tokens.consume_a_token().ident()?)?;
            tokens.discard_whitespace();
            let second = tokens.next_token().ident().and_then(side);
            side_or_corner = if let Some(second) = second {
                tokens.discard_a_token();
                corner(first, second)?
            } else {
                first
            };
        } else {
            has_direction = false;
        }
    }
    if interpolation_method.is_none() {
        tokens.discard_whitespace();
        interpolation_method = parse_color_interpolation_method(&mut tokens);
    }
    tokens.discard_whitespace();
    if !tokens.has_next_token() {
        return None;
    }
    if has_direction || interpolation_method.is_some() {
        if !tokens.next_token().is_comma() {
            return None;
        }
        tokens.discard_a_token();
    }
    let (color_stop_list, color_syntax) = parse_color_stop_list(context, property, &mut tokens, false)?;
    Some(StyleValueData::LinearGradient {
        has_direction_value: direction_value.is_some(),
        direction_value: optional_retained(direction_value),
        side_or_corner,
        color_stop_list,
        gradient_type: u8::from(webkit.is_some()),
        repeating: repeating.is_some(),
        color_interpolation_method: optional_retained(interpolation_method),
        color_syntax,
    })
}

// https://drafts.csswg.org/css-images-4/#conic-gradients
// conic-gradient( [ [ [ from [ <angle> | <zero> ] ]? [ at <position> ]? ] || <color-interpolation-method> ]? , <angular-color-stop-list> )
fn parse_conic_gradient(
    context: &ParseContext,
    property: u16,
    name: &[u16],
    arguments: &[ComponentValue],
) -> Option<StyleValueData> {
    let (repeating, name) = strip_ascii_prefix(name, b"repeating-").map_or((false, name), |name| (true, name));
    if !equals_ascii_case_insensitive(name, b"conic-gradient") {
        return None;
    }
    let mut tokens = TokenStream::new(arguments);
    tokens.discard_whitespace();
    if !tokens.has_next_token() {
        return None;
    }
    let mut from_angle = None;
    let mut position = None;
    let mut interpolation_method = None;
    while let Some(identifier) = tokens.next_token().ident() {
        if equals_ascii_case_insensitive(identifier, b"from") {
            if from_angle.is_some() || position.is_some() {
                return None;
            }
            tokens.discard_a_token();
            tokens.discard_whitespace();
            let mut angle_tokens = tokens.clone();
            from_angle = parse_angle_from_stream(context, property, &mut angle_tokens, NumericRange::INFINITE);
            if from_angle.is_some() {
                tokens = angle_tokens;
            } else if matches!(tokens.next_token().kind, ComponentKind::Token(ParserTokenKind::Number { value, .. }) if value == 0.0)
            {
                tokens.discard_a_token();
                from_angle = Some(StyleValueData::Angle { value: 0.0, unit: 0 });
            } else {
                return None;
            }
        } else if equals_ascii_case_insensitive(identifier, b"at") {
            if position.is_some() {
                return None;
            }
            tokens.discard_a_token();
            position = Some(parse_position_from_stream(context, property, &mut tokens, false)?);
        } else if equals_ascii_case_insensitive(identifier, b"in") {
            if interpolation_method.is_some() {
                return None;
            }
            interpolation_method = Some(parse_color_interpolation_method(&mut tokens)?);
        } else {
            break;
        }
        tokens.discard_whitespace();
        if !tokens.has_next_token() {
            return None;
        }
    }
    if from_angle.is_some() || position.is_some() || interpolation_method.is_some() {
        if !tokens.next_token().is_comma() {
            return None;
        }
        tokens.discard_a_token();
    }
    let (color_stop_list, color_syntax) = parse_color_stop_list(context, property, &mut tokens, true)?;
    Some(StyleValueData::ConicGradient {
        from_angle: optional_retained(from_angle),
        position: retained(position.unwrap_or_else(center_position)),
        color_stop_list,
        repeating,
        color_interpolation_method: optional_retained(interpolation_method),
        color_syntax,
    })
}

fn default_radial_size() -> StyleValueData {
    StyleValueData::RadialSize {
        component_count: 1,
        is_extent_0: true,
        extent_0: 2,
        value_0: RetainedStyleValueData::none(),
        is_extent_1: false,
        extent_1: 0,
        value_1: RetainedStyleValueData::none(),
    }
}

fn parse_ending_shape(tokens: &mut TokenStream<'_>) -> Option<u8> {
    let start = tokens.current_index();
    tokens.discard_whitespace();
    let Some(identifier) = tokens.next_token().ident() else {
        tokens.position = start;
        return None;
    };
    let shape = if equals_ascii_case_insensitive(identifier, b"circle") {
        0
    } else if equals_ascii_case_insensitive(identifier, b"ellipse") {
        1
    } else {
        tokens.position = start;
        return None;
    };
    tokens.discard_a_token();
    Some(shape)
}

fn radial_size_shape(size: &StyleValueData) -> Option<(u8, bool)> {
    let StyleValueData::RadialSize {
        component_count,
        is_extent_0,
        ..
    } = size
    else {
        return None;
    };
    Some((*component_count, *is_extent_0))
}

// https://drafts.csswg.org/css-images-4/#radial-gradients
// <radial-gradient-syntax> = [ [ [ <radial-shape> || <radial-size> ]? [ at <position> ]? ] || <color-interpolation-method> ]? , <color-stop-list>
fn parse_radial_gradient(
    context: &ParseContext,
    property: u16,
    name: &[u16],
    arguments: &[ComponentValue],
) -> Option<StyleValueData> {
    let (repeating, name) = strip_ascii_prefix(name, b"repeating-").map_or((false, name), |name| (true, name));
    if !equals_ascii_case_insensitive(name, b"radial-gradient") {
        return None;
    }
    let mut tokens = TokenStream::new(arguments);
    tokens.discard_whitespace();
    if !tokens.has_next_token() {
        return None;
    }
    let mut expect_comma = false;
    let mut interpolation_method = parse_color_interpolation_method(&mut tokens);
    tokens.discard_whitespace();
    let mut ending_shape = parse_ending_shape(&mut tokens);
    let mut size = parse_radial_size(context, property, &mut tokens);
    if ending_shape.is_none() && size.is_some() {
        ending_shape = parse_ending_shape(&mut tokens);
    }
    if size.is_some() {
        expect_comma = true;
    }
    let size = size.take().unwrap_or_else(default_radial_size);
    let (component_count, first_is_extent) = radial_size_shape(&size)?;
    let ending_shape = if let Some(shape) = ending_shape {
        expect_comma = true;
        if (shape == 0 && component_count != 1) || (shape == 1 && component_count != 2 && !first_is_extent) {
            return None;
        }
        shape
    } else {
        u8::from(component_count != 1 || first_is_extent)
    };
    tokens.discard_whitespace();
    if !tokens.has_next_token() {
        return None;
    }
    let mut position = None;
    if tokens
        .next_token()
        .ident()
        .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"at"))
    {
        tokens.discard_a_token();
        position = Some(parse_position_from_stream(context, property, &mut tokens, false)?);
        expect_comma = true;
    }
    tokens.discard_whitespace();
    if interpolation_method.is_none() {
        interpolation_method = parse_color_interpolation_method(&mut tokens);
        tokens.discard_whitespace();
    }
    if interpolation_method.is_some() {
        expect_comma = true;
    }
    if !tokens.has_next_token() {
        return None;
    }
    if expect_comma {
        if !tokens.next_token().is_comma() {
            return None;
        }
        tokens.discard_a_token();
    }
    let (color_stop_list, color_syntax) = parse_color_stop_list(context, property, &mut tokens, false)?;
    Some(StyleValueData::RadialGradient {
        ending_shape,
        size: retained(size),
        position: retained(position.unwrap_or_else(center_position)),
        color_stop_list,
        repeating,
        color_interpolation_method: optional_retained(interpolation_method),
        color_syntax,
    })
}

fn parse_gradient(
    context: &ParseContext,
    property: u16,
    name: &[u16],
    arguments: &[ComponentValue],
) -> Option<StyleValueData> {
    parse_linear_gradient(context, property, name, arguments)
        .or_else(|| parse_conic_gradient(context, property, name, arguments))
        .or_else(|| parse_radial_gradient(context, property, name, arguments))
}

pub(crate) fn parse_image_value(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    allow_image_set: bool,
) -> Option<StyleValueData> {
    tokens.discard_whitespace();
    if let Some(url) = parse_url_value(context, tokens.next_token()) {
        let is_fragment = matches!(&url, StyleValueData::Url { url, .. } if url.as_bytes().starts_with(b"#"));
        if !is_fragment {
            tokens.discard_a_token();
            return image_from_url(context, url);
        }
    }
    if allow_image_set
        && let Some((name, arguments)) = tokens.next_token().function()
        && (equals_ascii_case_insensitive(name, b"image-set")
            || equals_ascii_case_insensitive(name, b"-webkit-image-set"))
    {
        let parsed = parse_image_set(context, property, arguments)?;
        tokens.discard_a_token();
        return Some(parsed);
    }
    if let Some((name, arguments)) = tokens.next_token().function()
        && let Some(parsed) = parse_gradient(context, property, name, arguments)
    {
        tokens.discard_a_token();
        return Some(parsed);
    }
    None
}

pub(crate) fn is_image_function_name(name: &[u16]) -> bool {
    [
        "image-set",
        "-webkit-image-set",
        "linear-gradient",
        "repeating-linear-gradient",
        "-webkit-linear-gradient",
        "-webkit-repeating-linear-gradient",
        "conic-gradient",
        "repeating-conic-gradient",
        "radial-gradient",
        "repeating-radial-gradient",
    ]
    .iter()
    .any(|expected| equals_ascii_case_insensitive(name, expected.as_bytes()))
}

fn parse_image_or_none(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(values);
    tokens.discard_whitespace();
    let parsed = if tokens
        .next_token()
        .ident()
        .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"none"))
    {
        tokens.discard_a_token();
        StyleValueData::Keyword {
            keyword: crate::css::css_enums::keyword::NONE,
        }
    } else if let Some(image) = parse_image_value(context, property, &mut tokens, true) {
        image
    } else if property == property_id::MASK_IMAGE {
        let url = parse_url_value(context, tokens.next_token())?;
        tokens.discard_a_token();
        url
    } else {
        return None;
    };
    tokens.discard_whitespace();
    (!tokens.has_next_token()).then_some(parsed)
}

pub(crate) fn parse_image_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if !matches!(
        property,
        property_id::BACKGROUND_IMAGE
            | property_id::MASK_IMAGE
            | property_id::LIST_STYLE_IMAGE
            | property_id::BORDER_IMAGE_SOURCE
    ) {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    }
    let parsed = if matches!(property, property_id::BACKGROUND_IMAGE | property_id::MASK_IMAGE) {
        values
            .split(ComponentValue::is_comma)
            .map(|item| parse_image_or_none(context, property, item))
            .collect::<Option<Vec<_>>>()
            .map(value_list)
    } else {
        parse_image_or_none(context, property, values)
    };
    parsed.map_or(ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED), |parsed| {
        ParseOutcome::Parsed(Arc::new(parsed))
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_tokenizer::tokenize_for_parser;
    use crate::css::parser::component_value::consume_a_list_of_component_values;

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
            intern_utf16_fly_string: None,
            normalize_svg_path_data: None,
            precomputed_svg_paths: std::ptr::null(),
            precomputed_svg_path_count: 0,
            font_format_is_supported: None,
            font_tech_is_supported: None,
            random_function_index: std::ptr::null_mut(),
        }
    }

    fn parse(property: u16, source: &str) -> ParseOutcome {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        parse_image_property(&context(), property, &values)
    }

    #[test]
    fn parses_url_images_and_image_sets() {
        for source in [
            "url(image.png)",
            "image-set(url(one.png) 1x, \"two.png\" 2x)",
            "-webkit-image-set(url(one.png), url(two.png) 2x)",
        ] {
            assert!(matches!(
                parse(property_id::BORDER_IMAGE_SOURCE, source),
                ParseOutcome::Parsed(_)
            ));
        }
        assert!(matches!(
            parse(property_id::MASK_IMAGE, "url(#mask)"),
            ParseOutcome::Parsed(_)
        ));
    }

    #[test]
    fn parses_image_lists_and_rejects_invalid_options() {
        assert!(matches!(
            parse(property_id::BACKGROUND_IMAGE, "none, url(image.png)"),
            ParseOutcome::Parsed(_)
        ));
        for source in ["image-set()", "image-set(url(one.png) 1x 2x)", "url(#mask)"] {
            assert!(matches!(
                parse(property_id::BACKGROUND_IMAGE, source),
                ParseOutcome::NotHandled(_)
            ));
        }
    }

    #[test]
    fn parses_linear_gradients() {
        for source in [
            "linear-gradient(#000, #fff)",
            "repeating-linear-gradient(to left top in oklab, #000 10% 20%, 30%, #fff)",
            "-webkit-linear-gradient(left, #000, #fff)",
            "-webkit-repeating-linear-gradient(45deg, #000, #fff)",
        ] {
            let ParseOutcome::Parsed(value) = parse(property_id::BACKGROUND_IMAGE, source) else {
                panic!("gradient should parse: {source}");
            };
            let StyleValueData::ValueList { values, .. } = &*value else {
                panic!("background-image should be a list");
            };
            assert!(matches!(
                values.as_slice()[0].data(),
                StyleValueData::LinearGradient { .. }
            ));
        }
    }

    #[test]
    fn parses_conic_and_radial_gradients() {
        for source in [
            "conic-gradient(from 0 at left top in hsl longer hue, #000 0, #fff 100%)",
            "repeating-conic-gradient(#000 0deg 20deg, 30deg, #fff)",
            "radial-gradient(circle closest-side at 20% 30% in oklab, #000, #fff)",
            "repeating-radial-gradient(ellipse 10px 20px, #000 10%, #fff 20%)",
        ] {
            assert!(
                matches!(parse(property_id::MASK_IMAGE, source), ParseOutcome::Parsed(_)),
                "{source}"
            );
        }
    }

    #[test]
    fn rejects_invalid_gradient_components() {
        for source in [
            "linear-gradient(10%, #000)",
            "linear-gradient(to left right, #000, #fff)",
            "conic-gradient(from 10px, #000, #fff)",
            "radial-gradient(circle 10px 20px, #000, #fff)",
            "radial-gradient(ellipse 10px, #000, #fff)",
        ] {
            assert!(matches!(
                parse(property_id::BORDER_IMAGE_SOURCE, source),
                ParseOutcome::NotHandled(_)
            ));
        }
    }
}
