/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS color value parsing, mirroring ValueParsing.cpp's color parsers.

use crate::css::calc::{FfiNumericType, simplify_parsed_calculation};
use crate::css::color_conversion;
use crate::css::color_resolution::system_color_for_keyword;
use crate::css::css_enums::{
    channel_keyword, hue_interpolation_method, keyword, keyword_from_ascii_case_insensitive,
    keyword_to_channel_keyword, keyword_to_hue_interpolation_method, keyword_to_polar_color_space,
    keyword_to_rectangular_color_space, rectangular_color_space,
};
use crate::css::css_tokenizer::{CssNumberType, ParserTokenKind};
use crate::css::named_colors::named_color_from_name;
use crate::css::parser::calc_parser::{CalcParseError, CalcParserContext, parse_a_calc_function_node};
use crate::css::parser::component_value::{ComponentKind, ComponentValue};
use crate::css::parser::token_stream::TokenStream;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style_value::{ColorBase, RetainedNumericRangeList, RetainedStyleValueData, StyleValueData};

use super::value_parser::{ParseContext, context_allows_random_functions, equals_ascii_case_insensitive};

const COLOR_SYNTAX_LEGACY: u8 = 0;
const COLOR_SYNTAX_MODERN: u8 = 1;
const VALUE_TYPE_ANGLE: u8 = 2;
const VALUE_TYPE_NUMBER: u8 = 27;
const VALUE_TYPE_PERCENTAGE: u8 = 31;

#[derive(Clone, Copy, PartialEq, Eq)]
enum NumericKind {
    Angle,
    Number,
    Percentage,
}

struct ParsedNumeric {
    value: StyleValueData,
    kind: NumericKind,
}

fn color_base(color_type: Option<u8>, syntax: u8) -> ColorBase {
    ColorBase {
        has_color_type: color_type.is_some(),
        color_type: color_type.unwrap_or(0),
        color_syntax: syntax,
    }
}

fn retained(value: StyleValueData) -> RetainedStyleValueData {
    RetainedStyleValueData::from_owned(value)
}

fn retained_optional(value: Option<StyleValueData>) -> RetainedStyleValueData {
    value.map_or_else(RetainedStyleValueData::none, retained)
}

fn retain_fly_string(context: &ParseContext, string: &[u16]) -> Option<RetainedUtf16FlyString> {
    let callback = context.intern_utf16_fly_string?;
    let raw = unsafe { callback(string.as_ptr(), string.len()) };
    Some(unsafe { RetainedUtf16FlyString::from_leaked_raw(raw) })
}

fn make_color_function(
    color_type: u8,
    channels: [StyleValueData; 3],
    alpha: Option<StyleValueData>,
    syntax: u8,
    name: Option<RetainedUtf16FlyString>,
    origin_color: Option<StyleValueData>,
) -> StyleValueData {
    let [channel_0, channel_1, channel_2] = channels;
    StyleValueData::ColorFunction {
        color_base: color_base(Some(color_type), syntax),
        channel_0: retained(channel_0),
        channel_1: retained(channel_1),
        channel_2: retained(channel_2),
        alpha: retained_optional(alpha),
        has_name: name.is_some(),
        name: name.unwrap_or_else(RetainedUtf16FlyString::none),
        origin_color: retained_optional(origin_color),
    }
}

fn make_legacy_color(context: &ParseContext, rgba: [u8; 4], name: Option<&[u16]>) -> Option<StyleValueData> {
    Some(make_color_function(
        color_conversion::RGB,
        [
            StyleValueData::Number {
                value: f64::from(rgba[0]),
            },
            StyleValueData::Number {
                value: f64::from(rgba[1]),
            },
            StyleValueData::Number {
                value: f64::from(rgba[2]),
            },
        ],
        Some(StyleValueData::Number {
            value: f64::from(rgba[3]) / 255.0,
        }),
        COLOR_SYNTAX_LEGACY,
        match name {
            Some(name) => Some(retain_fly_string(context, name)?),
            None => None,
        },
        None,
    ))
}

fn hex_nibble(value: u16) -> Option<u8> {
    match value {
        value if value == u16::from(b'0') || (u16::from(b'1')..=u16::from(b'9')).contains(&value) => {
            Some(u8::try_from(value - u16::from(b'0')).ok()?)
        }
        value if (u16::from(b'a')..=u16::from(b'f')).contains(&value) => {
            Some(10 + u8::try_from(value - u16::from(b'a')).ok()?)
        }
        value if (u16::from(b'A')..=u16::from(b'F')).contains(&value) => {
            Some(10 + u8::try_from(value - u16::from(b'A')).ok()?)
        }
        _ => None,
    }
}

fn parse_hex_color(value: &[u16]) -> Option<[u8; 4]> {
    let pair = |high, low| Some(hex_nibble(high)? << 4 | hex_nibble(low)?);
    match value {
        [r, g, b] => Some([hex_nibble(*r)? * 17, hex_nibble(*g)? * 17, hex_nibble(*b)? * 17, 255]),
        [r, g, b, a] => Some([
            hex_nibble(*r)? * 17,
            hex_nibble(*g)? * 17,
            hex_nibble(*b)? * 17,
            hex_nibble(*a)? * 17,
        ]),
        [r0, r1, g0, g1, b0, b1] => Some([pair(*r0, *r1)?, pair(*g0, *g1)?, pair(*b0, *b1)?, 255]),
        [r0, r1, g0, g1, b0, b1, a0, a1] => Some([pair(*r0, *r1)?, pair(*g0, *g1)?, pair(*b0, *b1)?, pair(*a0, *a1)?]),
        _ => None,
    }
}

fn quirky_hex_digits(component: &ComponentValue) -> Option<Vec<u16>> {
    match &component.kind {
        ComponentKind::Token(ParserTokenKind::Ident(identifier)) => Some(identifier.to_vec()),
        ComponentKind::Token(
            ParserTokenKind::Number { value, number_type }
            | ParserTokenKind::Dimension {
                value,
                number_type,
                unit: _,
            },
        ) if *number_type != CssNumberType::Number && *value >= 0.0 && *value <= i64::MAX as f64 => {
            let mut serialization = format!("{}", *value as i64).encode_utf16().collect::<Vec<_>>();
            if let ComponentKind::Token(ParserTokenKind::Dimension { unit, .. }) = &component.kind {
                serialization.extend_from_slice(unit);
            }
            while serialization.len() < 6 {
                serialization.insert(0, u16::from(b'0'));
            }
            Some(serialization)
        }
        _ => None,
    }
}

fn allowed_channels(channels: &[u8]) -> u64 {
    channels
        .iter()
        .chain([channel_keyword::ALPHA].iter())
        .fold(0, |bits, channel| bits | (1 << channel))
}

fn parse_calculated_numeric(
    context: &ParseContext,
    property: u16,
    value: &ComponentValue,
    allowed_color_channels: u64,
    accepted: &[NumericKind],
    accepted_range: (f64, f64),
) -> Option<ParsedNumeric> {
    let (name, values) = value.function()?;
    let root = match parse_a_calc_function_node(
        name,
        values,
        CalcParserContext {
            percentages_resolve_as: None,
            property,
            random_function_index: context.random_function_index,
            intern_utf16_fly_string: context.intern_utf16_fly_string,
            allowed_color_channels,
            allow_random_functions: context_allows_random_functions(context),
            parse_context: context,
        },
    ) {
        Ok(root) => root,
        Err(CalcParseError::Invalid | CalcParseError::NotHandled) => return None,
    };
    let (root, numeric_type) = simplify_parsed_calculation(root, None)?;
    let kind = accepted.iter().copied().find(|kind| match kind {
        NumericKind::Angle => numeric_type.matches_dimension(1, None),
        NumericKind::Number => numeric_type.matches_number(None),
        NumericKind::Percentage => numeric_type.matches_percentage(),
    })?;
    let value_type = match kind {
        NumericKind::Angle => VALUE_TYPE_ANGLE,
        NumericKind::Number => VALUE_TYPE_NUMBER,
        NumericKind::Percentage => VALUE_TYPE_PERCENTAGE,
    };
    Some(ParsedNumeric {
        value: StyleValueData::Calculated {
            rust_calculation: crate::css::calc::CalcNodeHandle::from_arc(root),
            resolve_as_is_number: false,
            resolve_as_base: 0,
            resolved_type: FfiNumericType::from_calc(Some(numeric_type)),
            has_percentages_resolve_as: false,
            percentages_resolve_as: 0,
            resolve_numbers_as_integers: false,
            accepted_ranges: RetainedNumericRangeList::from_single_numeric_range(
                value_type,
                accepted_range.0,
                accepted_range.1,
            ),
        },
        kind,
    })
}

fn parse_numeric(
    context: &ParseContext,
    property: u16,
    stream: &mut TokenStream<'_>,
    allowed_color_channels: u64,
    accepted: &[NumericKind],
    allow_none: bool,
) -> Option<ParsedNumeric> {
    stream.discard_whitespace();
    let value = stream.next_token();
    if let Some(identifier) = value.ident() {
        if allow_none && equals_ascii_case_insensitive(identifier, b"none") {
            let keyword = keyword_from_ascii_case_insensitive(identifier)?;
            stream.discard_a_token();
            return Some(ParsedNumeric {
                value: StyleValueData::Keyword { keyword },
                kind: NumericKind::Number,
            });
        }
        if let Some(keyword) = keyword_from_ascii_case_insensitive(identifier)
            && let Some(channel) = keyword_to_channel_keyword(keyword)
            && allowed_color_channels & (1 << channel) != 0
        {
            stream.discard_a_token();
            return Some(ParsedNumeric {
                value: StyleValueData::Keyword { keyword },
                kind: NumericKind::Number,
            });
        }
    }
    let parsed = match &value.kind {
        ComponentKind::Token(ParserTokenKind::Number { value, .. }) if accepted.contains(&NumericKind::Number) => {
            Some(ParsedNumeric {
                value: StyleValueData::Number {
                    value: value.clamp(f32::MIN as f64, f32::MAX as f64),
                },
                kind: NumericKind::Number,
            })
        }
        ComponentKind::Token(ParserTokenKind::Percentage { value, .. })
            if accepted.contains(&NumericKind::Percentage) =>
        {
            Some(ParsedNumeric {
                value: StyleValueData::Percentage {
                    value: value.clamp(f32::MIN as f64, f32::MAX as f64),
                },
                kind: NumericKind::Percentage,
            })
        }
        ComponentKind::Token(ParserTokenKind::Dimension { value, unit, .. })
            if accepted.contains(&NumericKind::Angle) =>
        {
            let unit = ["deg", "grad", "rad", "turn"]
                .iter()
                .position(|name| equals_ascii_case_insensitive(unit, name.as_bytes()))?;
            Some(ParsedNumeric {
                value: StyleValueData::Angle {
                    value: value.clamp(f32::MIN as f64, f32::MAX as f64),
                    unit: u8::try_from(unit).ok()?,
                },
                kind: NumericKind::Angle,
            })
        }
        ComponentKind::Function { .. } => parse_calculated_numeric(
            context,
            property,
            value,
            allowed_color_channels,
            accepted,
            (f32::MIN as f64, f32::MAX as f64),
        ),
        _ => None,
    }?;
    stream.discard_a_token();
    Some(parsed)
}

fn parse_number_percentage_none(
    context: &ParseContext,
    property: u16,
    stream: &mut TokenStream<'_>,
    allowed_color_channels: u64,
) -> Option<ParsedNumeric> {
    parse_numeric(
        context,
        property,
        stream,
        allowed_color_channels,
        &[NumericKind::Number, NumericKind::Percentage],
        true,
    )
}

fn parse_hue_none(
    context: &ParseContext,
    property: u16,
    stream: &mut TokenStream<'_>,
    allowed_color_channels: u64,
) -> Option<ParsedNumeric> {
    parse_numeric(
        context,
        property,
        stream,
        allowed_color_channels,
        &[NumericKind::Angle, NumericKind::Number],
        true,
    )
}

fn parse_alpha(
    context: &ParseContext,
    property: u16,
    stream: &mut TokenStream<'_>,
    allowed_color_channels: u64,
) -> Option<StyleValueData> {
    stream.discard_whitespace();
    if !stream.next_token().is_delim(b'/') {
        return None;
    }
    stream.discard_a_token();
    Some(parse_number_percentage_none(context, property, stream, allowed_color_channels)?.value)
}

fn parse_relative_origin(
    context: &ParseContext,
    property: u16,
    stream: &mut TokenStream<'_>,
    allow_quirky_color: bool,
) -> Result<Option<StyleValueData>, ()> {
    stream.discard_whitespace();
    if !stream
        .next_token()
        .ident()
        .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"from"))
    {
        return Ok(None);
    }
    stream.discard_a_token();
    stream.discard_whitespace();
    parse_color_value(context, property, stream, allow_quirky_color)
        .map(Some)
        .ok_or(())
}

fn parse_rgb_or_hsl(
    context: &ParseContext,
    property: u16,
    name: &[u16],
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let is_rgb = equals_ascii_case_insensitive(name, b"rgb") || equals_ascii_case_insensitive(name, b"rgba");
    let is_hsl = equals_ascii_case_insensitive(name, b"hsl") || equals_ascii_case_insensitive(name, b"hsla");
    if !is_rgb && !is_hsl {
        return None;
    }
    let mut stream = TokenStream::new(values);
    let origin_color = parse_relative_origin(context, property, &mut stream, false).ok()?;
    let channels = if is_rgb {
        [channel_keyword::R, channel_keyword::G, channel_keyword::B]
    } else {
        [channel_keyword::H, channel_keyword::S, channel_keyword::L]
    };
    let allowed = origin_color.as_ref().map_or(0, |_| allowed_channels(&channels));
    let first = if is_rgb {
        parse_number_percentage_none(context, property, &mut stream, allowed)?
    } else {
        parse_hue_none(context, property, &mut stream, allowed)?
    };
    stream.discard_whitespace();
    let legacy = origin_color.is_none() && stream.next_token().is_comma();
    let (second, third, alpha) = if legacy {
        if matches!(first.value, StyleValueData::Keyword { .. }) {
            return None;
        }
        stream.discard_a_token();
        let second = parse_numeric(
            context,
            property,
            &mut stream,
            0,
            if is_rgb {
                &[NumericKind::Number, NumericKind::Percentage]
            } else {
                &[NumericKind::Percentage]
            },
            false,
        )?;
        stream.discard_whitespace();
        if !stream.next_token().is_comma() {
            return None;
        }
        stream.discard_a_token();
        let third = parse_numeric(
            context,
            property,
            &mut stream,
            0,
            if is_rgb {
                &[NumericKind::Number, NumericKind::Percentage]
            } else {
                &[NumericKind::Percentage]
            },
            false,
        )?;
        if is_rgb && (first.kind != second.kind || first.kind != third.kind) {
            return None;
        }
        stream.discard_whitespace();
        let alpha = if stream.has_next_token() {
            if !stream.next_token().is_comma() {
                return None;
            }
            stream.discard_a_token();
            Some(
                parse_numeric(
                    context,
                    property,
                    &mut stream,
                    0,
                    &[NumericKind::Number, NumericKind::Percentage],
                    false,
                )?
                .value,
            )
        } else {
            None
        };
        (second.value, third.value, alpha)
    } else {
        let second = parse_number_percentage_none(context, property, &mut stream, allowed)?.value;
        let third = parse_number_percentage_none(context, property, &mut stream, allowed)?.value;
        stream.discard_whitespace();
        let alpha = if stream.has_next_token() {
            Some(parse_alpha(context, property, &mut stream, allowed)?)
        } else {
            None
        };
        (second, third, alpha)
    };
    stream.discard_whitespace();
    if stream.has_next_token() {
        return None;
    }
    Some(make_color_function(
        if is_rgb {
            color_conversion::RGB
        } else {
            color_conversion::HSL
        },
        [first.value, second, third],
        alpha,
        if legacy {
            COLOR_SYNTAX_LEGACY
        } else {
            COLOR_SYNTAX_MODERN
        },
        None,
        origin_color,
    ))
}

fn parse_modern_function(
    context: &ParseContext,
    property: u16,
    name: &[u16],
    values: &[ComponentValue],
    allow_quirky_color: bool,
) -> Option<StyleValueData> {
    let (color_type, hue_channel, channels) = if equals_ascii_case_insensitive(name, b"hwb") {
        (
            color_conversion::HWB,
            true,
            [channel_keyword::H, channel_keyword::W, channel_keyword::B],
        )
    } else if equals_ascii_case_insensitive(name, b"lab") {
        (
            color_conversion::LAB,
            false,
            [channel_keyword::L, channel_keyword::A, channel_keyword::B],
        )
    } else if equals_ascii_case_insensitive(name, b"oklab") {
        (
            color_conversion::OKLAB,
            false,
            [channel_keyword::L, channel_keyword::A, channel_keyword::B],
        )
    } else if equals_ascii_case_insensitive(name, b"lch") {
        (
            color_conversion::LCH,
            true,
            [channel_keyword::L, channel_keyword::C, channel_keyword::H],
        )
    } else if equals_ascii_case_insensitive(name, b"oklch") {
        (
            color_conversion::OKLCH,
            true,
            [channel_keyword::L, channel_keyword::C, channel_keyword::H],
        )
    } else {
        return None;
    };
    let mut stream = TokenStream::new(values);
    let origin_color = parse_relative_origin(context, property, &mut stream, allow_quirky_color).ok()?;
    let allowed = origin_color.as_ref().map_or(0, |_| allowed_channels(&channels));
    let first = if hue_channel && color_type == color_conversion::HWB {
        parse_hue_none(context, property, &mut stream, allowed)?.value
    } else {
        parse_number_percentage_none(context, property, &mut stream, allowed)?.value
    };
    let second = parse_number_percentage_none(context, property, &mut stream, allowed)?.value;
    let third = if hue_channel && color_type != color_conversion::HWB {
        parse_hue_none(context, property, &mut stream, allowed)?.value
    } else {
        parse_number_percentage_none(context, property, &mut stream, allowed)?.value
    };
    stream.discard_whitespace();
    let alpha = if stream.has_next_token() {
        Some(parse_alpha(context, property, &mut stream, allowed)?)
    } else {
        None
    };
    stream.discard_whitespace();
    if stream.has_next_token() {
        return None;
    }
    Some(make_color_function(
        color_type,
        [first, second, third],
        alpha,
        COLOR_SYNTAX_MODERN,
        None,
        origin_color,
    ))
}

fn color_function_type(name: &[u16]) -> Option<(u8, [u8; 3])> {
    let rgb = [channel_keyword::R, channel_keyword::G, channel_keyword::B];
    let xyz = [channel_keyword::X, channel_keyword::Y, channel_keyword::Z];
    if equals_ascii_case_insensitive(name, b"srgb") {
        Some((color_conversion::SRGB, rgb))
    } else if equals_ascii_case_insensitive(name, b"srgb-linear") {
        Some((color_conversion::SRGB_LINEAR, rgb))
    } else if equals_ascii_case_insensitive(name, b"display-p3") {
        Some((color_conversion::DISPLAY_P3, rgb))
    } else if equals_ascii_case_insensitive(name, b"display-p3-linear") {
        Some((color_conversion::DISPLAY_P3_LINEAR, rgb))
    } else if equals_ascii_case_insensitive(name, b"a98-rgb") {
        Some((color_conversion::A98_RGB, rgb))
    } else if equals_ascii_case_insensitive(name, b"prophoto-rgb") {
        Some((color_conversion::PROPHOTO_RGB, rgb))
    } else if equals_ascii_case_insensitive(name, b"rec2020") {
        Some((color_conversion::REC2020, rgb))
    } else if equals_ascii_case_insensitive(name, b"xyz-d50") {
        Some((color_conversion::XYZ_D50, xyz))
    } else if equals_ascii_case_insensitive(name, b"xyz") || equals_ascii_case_insensitive(name, b"xyz-d65") {
        Some((color_conversion::XYZ_D65, xyz))
    } else {
        None
    }
}

fn parse_color_function(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let mut stream = TokenStream::new(values);
    let origin_color = parse_relative_origin(context, property, &mut stream, false).ok()?;
    stream.discard_whitespace();
    let space_name = stream.next_token().ident()?;
    let (color_type, channels) = color_function_type(space_name)?;
    stream.discard_a_token();
    let allowed = origin_color.as_ref().map_or(0, |_| allowed_channels(&channels));
    let channel_0 = parse_number_percentage_none(context, property, &mut stream, allowed)?.value;
    let channel_1 = parse_number_percentage_none(context, property, &mut stream, allowed)?.value;
    let channel_2 = parse_number_percentage_none(context, property, &mut stream, allowed)?.value;
    stream.discard_whitespace();
    let alpha = if stream.has_next_token() {
        Some(parse_alpha(context, property, &mut stream, allowed)?)
    } else {
        None
    };
    stream.discard_whitespace();
    if stream.has_next_token() {
        return None;
    }
    Some(make_color_function(
        color_type,
        [channel_0, channel_1, channel_2],
        alpha,
        COLOR_SYNTAX_MODERN,
        None,
        origin_color,
    ))
}

pub(crate) fn parse_color_interpolation_method(stream: &mut TokenStream<'_>) -> Option<StyleValueData> {
    let start = stream.current_index();
    stream.discard_whitespace();
    if !stream
        .next_token()
        .ident()
        .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"in"))
    {
        stream.position = start;
        return None;
    }
    stream.discard_a_token();
    stream.discard_whitespace();
    let identifier = stream.next_token().ident()?;
    let keyword = keyword_from_ascii_case_insensitive(identifier)?;
    stream.discard_a_token();
    if let Some(mut color_space) = keyword_to_rectangular_color_space(keyword) {
        if color_space == rectangular_color_space::XYZ {
            color_space = rectangular_color_space::XYZ_D65;
        }
        return Some(StyleValueData::ColorInterpolationMethod {
            is_polar: false,
            color_space,
            hue_interpolation_method: 0,
        });
    }
    let color_space = keyword_to_polar_color_space(keyword)?;
    stream.discard_whitespace();
    let mut method = hue_interpolation_method::SHORTER;
    if let Some(identifier) = stream.next_token().ident()
        && let Some(keyword) = keyword_from_ascii_case_insensitive(identifier)
    {
        method = keyword_to_hue_interpolation_method(keyword)?;
        stream.discard_a_token();
        stream.discard_whitespace();
        if !stream
            .next_token()
            .ident()
            .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"hue"))
        {
            return None;
        }
        stream.discard_a_token();
    }
    Some(StyleValueData::ColorInterpolationMethod {
        is_polar: true,
        color_space,
        hue_interpolation_method: method,
    })
}

struct MixComponent {
    color: StyleValueData,
    percentage: Option<StyleValueData>,
}

fn parse_mix_percentage(context: &ParseContext, property: u16, stream: &mut TokenStream<'_>) -> Option<StyleValueData> {
    let start = stream.current_index();
    let mut parsed = parse_numeric(context, property, stream, 0, &[NumericKind::Percentage], false)?;
    match &mut parsed.value {
        StyleValueData::Percentage { value } if !(0.0..=100.0).contains(value) => {
            stream.position = start;
            return None;
        }
        StyleValueData::Calculated { accepted_ranges, .. } => {
            *accepted_ranges = RetainedNumericRangeList::from_single_numeric_range(VALUE_TYPE_PERCENTAGE, 0.0, 100.0);
        }
        _ => {}
    }
    Some(parsed.value)
}

fn parse_mix_component(context: &ParseContext, property: u16, stream: &mut TokenStream<'_>) -> Option<MixComponent> {
    stream.discard_whitespace();
    let mut percentage = parse_mix_percentage(context, property, stream);
    stream.discard_whitespace();
    let color = parse_color_value(context, property, stream, false)?;
    stream.discard_whitespace();
    if percentage.is_none() {
        percentage = parse_mix_percentage(context, property, stream);
        stream.discard_whitespace();
    }
    Some(MixComponent { color, percentage })
}

fn is_zero_percentage(value: Option<&StyleValueData>) -> bool {
    matches!(value, Some(StyleValueData::Percentage { value }) if *value == 0.0)
}

fn parse_color_mix(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let mut stream = TokenStream::new(values);
    let method_start = stream.current_index();
    let method = parse_color_interpolation_method(&mut stream);
    if method.is_some() {
        stream.discard_whitespace();
        if !stream.next_token().is_comma() {
            return None;
        }
        stream.discard_a_token();
    } else {
        stream.position = method_start;
    }
    let first = parse_mix_component(context, property, &mut stream)?;
    if !stream.next_token().is_comma() {
        return None;
    }
    stream.discard_a_token();
    let second = parse_mix_component(context, property, &mut stream)?;
    if is_zero_percentage(first.percentage.as_ref()) && is_zero_percentage(second.percentage.as_ref()) {
        return None;
    }
    stream.discard_whitespace();
    if stream.has_next_token() {
        return None;
    }
    Some(StyleValueData::ColorMix {
        color_base: color_base(None, COLOR_SYNTAX_MODERN),
        color_interpolation_method: retained_optional(method),
        first_color: retained(first.color),
        first_percentage: retained_optional(first.percentage),
        second_color: retained(second.color),
        second_percentage: retained_optional(second.percentage),
    })
}

fn parse_light_dark_or_contrast(
    context: &ParseContext,
    property: u16,
    name: &[u16],
    values: &[ComponentValue],
    allow_quirky_color: bool,
) -> Option<StyleValueData> {
    let mut stream = TokenStream::new(values);
    let first = parse_color_value(context, property, &mut stream, allow_quirky_color)?;
    stream.discard_whitespace();
    if equals_ascii_case_insensitive(name, b"contrast-color") {
        if stream.has_next_token() {
            return None;
        }
        return Some(StyleValueData::ContrastColor {
            color_base: color_base(None, COLOR_SYNTAX_MODERN),
            color: retained(first),
        });
    }
    if !equals_ascii_case_insensitive(name, b"light-dark") || !stream.next_token().is_comma() {
        return None;
    }
    stream.discard_a_token();
    let second = parse_color_value(context, property, &mut stream, allow_quirky_color)?;
    stream.discard_whitespace();
    if stream.has_next_token() {
        return None;
    }
    Some(StyleValueData::LightDark {
        color_base: color_base(None, COLOR_SYNTAX_MODERN),
        light: retained(first),
        dark: retained(second),
    })
}

fn parse_color_function_component(
    context: &ParseContext,
    property: u16,
    name: &[u16],
    values: &[ComponentValue],
    allow_quirky_color: bool,
) -> Option<StyleValueData> {
    if equals_ascii_case_insensitive(name, b"color") {
        parse_color_function(context, property, values)
    } else if equals_ascii_case_insensitive(name, b"color-mix") {
        parse_color_mix(context, property, values)
    } else if equals_ascii_case_insensitive(name, b"rgb")
        || equals_ascii_case_insensitive(name, b"rgba")
        || equals_ascii_case_insensitive(name, b"hsl")
        || equals_ascii_case_insensitive(name, b"hsla")
    {
        parse_rgb_or_hsl(context, property, name, values)
    } else if equals_ascii_case_insensitive(name, b"hwb")
        || equals_ascii_case_insensitive(name, b"lab")
        || equals_ascii_case_insensitive(name, b"lch")
        || equals_ascii_case_insensitive(name, b"oklab")
        || equals_ascii_case_insensitive(name, b"oklch")
    {
        parse_modern_function(context, property, name, values, allow_quirky_color)
    } else if equals_ascii_case_insensitive(name, b"light-dark")
        || equals_ascii_case_insensitive(name, b"contrast-color")
    {
        parse_light_dark_or_contrast(context, property, name, values, allow_quirky_color)
    } else {
        None
    }
}

pub(crate) fn is_color_function_name(name: &[u16]) -> bool {
    [
        "color",
        "color-mix",
        "contrast-color",
        "hsl",
        "hsla",
        "hwb",
        "lab",
        "lch",
        "light-dark",
        "oklab",
        "oklch",
        "rgb",
        "rgba",
    ]
    .iter()
    .any(|expected| equals_ascii_case_insensitive(name, expected.as_bytes()))
}

pub(crate) fn parse_color_value(
    context: &ParseContext,
    property: u16,
    stream: &mut TokenStream<'_>,
    allow_quirky_color: bool,
) -> Option<StyleValueData> {
    stream.discard_whitespace();
    let value = stream.next_token();
    if let Some(identifier) = value.ident() {
        if let Some(keyword) = keyword_from_ascii_case_insensitive(identifier)
            && (keyword == keyword::CURRENTCOLOR
                || system_color_for_keyword(keyword, false).is_some()
                || system_color_for_keyword(keyword, true).is_some())
        {
            stream.discard_a_token();
            return Some(StyleValueData::Keyword { keyword });
        }
        if equals_ascii_case_insensitive(identifier, b"transparent") {
            // NB: The retained name only preserves source spelling during serialization. A
            //     callback-free worker parse can omit it without changing computed color semantics.
            let name = context.intern_utf16_fly_string.is_some().then_some(identifier);
            let color = make_legacy_color(context, [0, 0, 0, 0], name)?;
            stream.discard_a_token();
            return Some(color);
        }
        if let Some(rgba) = named_color_from_name(identifier) {
            let name = context.intern_utf16_fly_string.is_some().then_some(identifier);
            let color = make_legacy_color(context, rgba, name)?;
            stream.discard_a_token();
            return Some(color);
        }
    }
    if let ComponentKind::Function { name, values } = &value.kind
        && let Some(color) = parse_color_function_component(context, property, name, values, allow_quirky_color)
    {
        stream.discard_a_token();
        return Some(color);
    }
    if let ComponentKind::Token(ParserTokenKind::Hash { value, .. }) = &value.kind {
        let color = make_legacy_color(context, parse_hex_color(value)?, None)?;
        stream.discard_a_token();
        return Some(color);
    }
    if allow_quirky_color {
        let digits = quirky_hex_digits(value)?;
        if !matches!(digits.len(), 3 | 6) {
            return None;
        }
        let color = make_legacy_color(context, parse_hex_color(&digits)?, None)?;
        stream.discard_a_token();
        return Some(color);
    }
    None
}

pub(crate) fn color_syntax(value: &StyleValueData) -> u8 {
    match value {
        StyleValueData::ContrastColor { color_base, .. }
        | StyleValueData::ColorFunction { color_base, .. }
        | StyleValueData::ColorMix { color_base, .. }
        | StyleValueData::LightDark { color_base, .. } => color_base.color_syntax,
        _ => COLOR_SYNTAX_LEGACY,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_tokenizer::tokenize_for_parser;
    use crate::css::parser::component_value::consume_a_list_of_component_values;

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
            random_function_index: std::ptr::null_mut(),
        }
    }

    fn parse(source: &str) -> Option<StyleValueData> {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        let mut stream = TokenStream::new(&values);
        let color = parse_color_value(&context(), 1, &mut stream, false)?;
        stream.discard_whitespace();
        (!stream.has_next_token()).then_some(color)
    }

    #[test]
    fn parses_color_literals_and_keywords() {
        for source in [
            "red",
            "transparent",
            "currentcolor",
            "#123",
            "#1234",
            "#112233",
            "#11223344",
        ] {
            assert!(parse(source).is_some(), "{source}");
        }
        for source in ["unknown", "#12", "#xyz"] {
            assert!(parse(source).is_none(), "{source}");
        }
    }

    #[test]
    fn parses_color_functions() {
        for source in [
            "rgb(1 2 3 / 50%)",
            "rgb(1, 2, 3, .5)",
            "hsl(120 100% 50%)",
            "hwb(120 0% 0%)",
            "lab(50% 0 0)",
            "lch(50% 20 30deg)",
            "oklab(0.5 0 0)",
            "oklch(0.5 0.2 30)",
            "color(display-p3 1 0 0)",
            "color(display-p3-linear 1 0 0)",
            "color-mix(in srgb, red 20%, blue)",
            "light-dark(white, black)",
            "contrast-color(white)",
        ] {
            assert!(parse(source).is_some(), "{source}");
        }
    }

    #[test]
    fn parses_relative_colors_and_channel_calculations() {
        for source in [
            "rgb(from red r g b / alpha)",
            "hsl(from red calc(h + 10) s l)",
            "lab(from red l calc(a * 2) b)",
            "color(from red srgb r g b)",
        ] {
            assert!(parse(source).is_some(), "{source}");
        }
        assert!(parse("rgb(calc(r + 1) 0 0)").is_none());
    }

    #[test]
    fn rejects_malformed_color_functions() {
        for source in [
            "rgb(1 2)",
            "rgb(1, 2%, 3)",
            "hsl(0, 1, 2%)",
            "color(unknown 1 2 3)",
            "color-mix(in srgb, red 0%, blue 0%)",
            "light-dark(red)",
            "contrast-color(red, blue)",
        ] {
            assert!(parse(source).is_none(), "{source}");
        }
    }
}
