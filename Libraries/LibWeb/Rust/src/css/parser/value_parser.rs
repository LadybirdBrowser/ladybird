/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Parsed values use the thread-confined shared graph owned by the C++ style objects.
#![allow(clippy::arc_with_non_send_sync)]

use crate::css::css_enums::{
    keyword, keyword_from_ascii_case_insensitive, keyword_to_cross_origin_modifier_value,
    keyword_to_referrer_policy_modifier_value,
};
use crate::css::css_pixels::CssPixels;
use crate::css::css_tokenizer::{CssNumberType, ParserTokenKind, tokenize_for_parser};
use crate::css::display::FfiDisplay;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::component_value::{ComponentKind, ComponentValue, consume_a_list_of_component_values};
use crate::css::property_metadata::{
    FIRST_SHORTHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, property_accepted_keywords, property_accepted_value_types,
    property_accepts_only_keywords, property_custom_ident_blacklist, property_has_coordinating_list_multiplicity,
    property_has_unitless_length_quirk, property_id, property_is_shorthand, property_numeric_ranges,
    property_percentages_resolve_to, property_resolve_legacy_value_alias,
};
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style_compute::{LENGTH_UNIT_NAMES, px_length_unit};
use crate::css::style_value::{
    RetainedRequestUrlModifier, RetainedRequestUrlModifierList, RetainedString, RetainedStyleValueData,
    RetainedStyleValueDataList, StyleValueData,
};
use std::collections::BTreeMap;
use std::ffi::c_void;
use std::sync::{Arc, Mutex, OnceLock};

include!(concat!(env!("OUT_DIR"), "/dimension_units_generated.rs"));

const PROPERTY_NOT_PORTED: NotHandledReason = NotHandledReason {
    label: "property:not-ported",
    c_label: b"property:not-ported\0",
};
const COMPONENT_VALUES_INVALID: NotHandledReason = NotHandledReason {
    label: "component-values:invalid",
    c_label: b"component-values:invalid\0",
};
const INVALID_FFI_INPUT: NotHandledReason = NotHandledReason {
    label: "ffi:invalid-input",
    c_label: b"ffi:invalid-input\0",
};
const CALC_NOT_PORTED: NotHandledReason = NotHandledReason {
    label: "calc",
    c_label: b"calc\0",
};
const SUBSTITUTION_NOT_PORTED: NotHandledReason = NotHandledReason {
    label: "substitution",
    c_label: b"substitution\0",
};
const FUNCTION_NOT_PORTED: NotHandledReason = NotHandledReason {
    label: "function:not-ported",
    c_label: b"function:not-ported\0",
};

// NB: Keep these in the order of the C++ ValueType enum.
const VALUE_TYPE_ANGLE: u8 = 2;
const VALUE_TYPE_CUSTOM_IDENT: u8 = 10;
const VALUE_TYPE_DASHED_IDENT: u8 = 11;
const VALUE_TYPE_FLEX: u8 = 15;
const VALUE_TYPE_FREQUENCY: u8 = 21;
const VALUE_TYPE_IMAGE: u8 = 23;
const VALUE_TYPE_INTEGER: u8 = 24;
const VALUE_TYPE_LENGTH: u8 = 25;
const VALUE_TYPE_NUMBER: u8 = 27;
const VALUE_TYPE_OPACITY_VALUE: u8 = 28;
const VALUE_TYPE_PERCENTAGE: u8 = 31;
const VALUE_TYPE_RATIO: u8 = 33;
const VALUE_TYPE_RESOLUTION: u8 = 35;
const VALUE_TYPE_STRING: u8 = 37;
const VALUE_TYPE_TIME: u8 = 38;
const VALUE_TYPE_URL: u8 = 42;

const PORTED_TEXT_VALUE_TYPES: [u8; 4] = [
    VALUE_TYPE_CUSTOM_IDENT,
    VALUE_TYPE_DASHED_IDENT,
    VALUE_TYPE_STRING,
    VALUE_TYPE_URL,
];

const PORTED_NUMERIC_VALUE_TYPES: [u8; 11] = [
    VALUE_TYPE_ANGLE,
    VALUE_TYPE_FLEX,
    VALUE_TYPE_FREQUENCY,
    VALUE_TYPE_INTEGER,
    VALUE_TYPE_LENGTH,
    VALUE_TYPE_NUMBER,
    VALUE_TYPE_OPACITY_VALUE,
    VALUE_TYPE_PERCENTAGE,
    VALUE_TYPE_RATIO,
    VALUE_TYPE_RESOLUTION,
    VALUE_TYPE_TIME,
];

pub(crate) struct NotHandledReason {
    label: &'static str,
    c_label: &'static [u8],
}

/// The C++ value-parsing contexts which affect grammar decisions.
#[repr(u8)]
#[derive(Clone, Copy)]
#[allow(dead_code)]
pub enum FfiValueParsingContextKind {
    Property,
    Function,
    Descriptor,
    Special,
    RelativeColor,
}

/// One entry in the C++ Parser's value-context stack.
#[repr(C)]
pub struct FfiValueParsingContext {
    pub kind: FfiValueParsingContextKind,
    /// PropertyID, SpecialContext, or AtRuleID, depending on `kind`.
    pub value: u16,
    /// DescriptorID when `kind` is Descriptor.
    pub secondary_value: u16,
    /// Function name when `kind` is Function.
    pub name: FfiUtf16View,
    /// The RelativeColorParseContext allowed-channel bitmap.
    pub allowed_channels: u64,
}

/// Parser state required by CSS value parsing.
#[repr(C)]
pub struct ParseContext {
    pub in_quirks_mode: bool,
    pub is_svg_presentation_attribute: bool,
    pub value_contexts: *const FfiValueParsingContext,
    pub value_context_count: usize,
    pub document_url: *const u8,
    pub document_url_length: usize,
    pub document_base_url: *const u8,
    pub document_base_url_length: usize,
    pub intern_utf16_fly_string: Option<unsafe extern "C" fn(*const u16, usize) -> usize>,
}

pub(crate) enum ParseOutcome {
    Parsed(Arc<StyleValueData>),
    Invalid,
    NotHandled(&'static NotHandledReason),
}

fn single_non_whitespace_value(values: &[ComponentValue]) -> Option<&ComponentValue> {
    let mut values = values.iter().filter(|value| !value.is_whitespace());
    let value = values.next()?;
    values.next().is_none().then_some(value)
}

fn parse_builtin_value(values: &[ComponentValue]) -> Option<StyleValueData> {
    let keyword = keyword_from_ascii_case_insensitive(single_non_whitespace_value(values)?.ident()?)?;
    matches!(
        keyword,
        keyword::INHERIT | keyword::INITIAL | keyword::UNSET | keyword::REVERT | keyword::REVERT_LAYER
    )
    .then_some(StyleValueData::Keyword { keyword })
}

fn property_uses_special_keyword_parser(property: u16) -> bool {
    // NB: These properties precede the generic parse_css_value_for_property()
    //     path in the C++ parse_css_value_in_cpp() switch. Their single-keyword
    //     results can have extra grammar constraints or specialized value types.
    matches!(
        property,
        property_id::ALIGN_ITEMS
            | property_id::ALIGN_SELF
            | property_id::ANCHOR_NAME
            | property_id::ANCHOR_SCOPE
            | property_id::ASPECT_RATIO
            | property_id::BACKDROP_FILTER
            | property_id::BACKGROUND_POSITION_X
            | property_id::BACKGROUND_POSITION_Y
            | property_id::BACKGROUND_REPEAT
            | property_id::BACKGROUND_SIZE
            | property_id::BORDER_BOTTOM_LEFT_RADIUS
            | property_id::BORDER_BOTTOM_RIGHT_RADIUS
            | property_id::BORDER_END_END_RADIUS
            | property_id::BORDER_END_START_RADIUS
            | property_id::BORDER_IMAGE_SLICE
            | property_id::BORDER_START_END_RADIUS
            | property_id::BORDER_START_START_RADIUS
            | property_id::BORDER_TOP_LEFT_RADIUS
            | property_id::BORDER_TOP_RIGHT_RADIUS
            | property_id::BOX_SHADOW
            | property_id::COLOR_SCHEME
            | property_id::CONTAIN
            | property_id::CONTAINER_NAME
            | property_id::CONTAINER_TYPE
            | property_id::CONTENT
            | property_id::COUNTER_INCREMENT
            | property_id::COUNTER_RESET
            | property_id::COUNTER_SET
            | property_id::CURSOR
            | property_id::DISPLAY
            | property_id::FILTER
            | property_id::FLEX
            | property_id::FONT_FAMILY
            | property_id::FONT_FEATURE_SETTINGS
            | property_id::FONT_LANGUAGE_OVERRIDE
            | property_id::FONT_VARIATION_SETTINGS
            | property_id::GRID
            | property_id::GRID_AREA
            | property_id::GRID_AUTO_COLUMNS
            | property_id::GRID_AUTO_ROWS
            | property_id::GRID_COLUMN
            | property_id::GRID_COLUMN_END
            | property_id::GRID_COLUMN_START
            | property_id::GRID_ROW
            | property_id::GRID_ROW_END
            | property_id::GRID_ROW_START
            | property_id::GRID_TEMPLATE
            | property_id::GRID_TEMPLATE_AREAS
            | property_id::GRID_TEMPLATE_COLUMNS
            | property_id::GRID_TEMPLATE_ROWS
            | property_id::JUSTIFY_ITEMS
            | property_id::JUSTIFY_SELF
            | property_id::MASK_REPEAT
            | property_id::MASK_SIZE
            | property_id::MATH_DEPTH
            | property_id::OVERFLOW
            | property_id::OVERFLOW_CLIP_MARGIN_BLOCK_END
            | property_id::OVERFLOW_CLIP_MARGIN_BLOCK_START
            | property_id::OVERFLOW_CLIP_MARGIN_BOTTOM
            | property_id::OVERFLOW_CLIP_MARGIN_INLINE_END
            | property_id::OVERFLOW_CLIP_MARGIN_INLINE_START
            | property_id::OVERFLOW_CLIP_MARGIN_LEFT
            | property_id::OVERFLOW_CLIP_MARGIN_RIGHT
            | property_id::OVERFLOW_CLIP_MARGIN_TOP
            | property_id::PAINT_ORDER
            | property_id::POSITION
            | property_id::POSITION_ANCHOR
            | property_id::POSITION_AREA
            | property_id::POSITION_TRY_FALLBACKS
            | property_id::POSITION_VISIBILITY
            | property_id::QUOTES
            | property_id::ROTATE
            | property_id::SCALE
            | property_id::TEXT_DECORATION_LINE
            | property_id::TEXT_INDENT
            | property_id::TEXT_SHADOW
            | property_id::TEXT_UNDERLINE_POSITION
            | property_id::TOUCH_ACTION
            | property_id::TRANSFORM
            | property_id::TRANSFORM_ORIGIN
            | property_id::TRANSITION_PROPERTY
            | property_id::TRANSLATE
            | property_id::WHITE_SPACE
            | property_id::WHITE_SPACE_TRIM
            | property_id::WILL_CHANGE
    )
}

#[derive(Clone, Copy)]
struct NumericRange {
    min: f64,
    max: f64,
}

impl NumericRange {
    const INFINITE: Self = Self {
        min: f32::MIN as f64,
        max: f32::MAX as f64,
    };
    const NON_NEGATIVE: Self = Self {
        min: 0.0,
        max: f32::MAX as f64,
    };

    fn contains(self, value: f64) -> bool {
        (self.min..=self.max).contains(&value)
    }
}

fn accepted_range(property: u16, value_type: u8) -> NumericRange {
    property_numeric_ranges(property)
        .iter()
        .find(|range| range.value_type == value_type)
        .map(|range| NumericRange {
            min: range.min,
            max: range.max,
        })
        .expect("numeric property value types must have an accepted range")
}

fn clamp_to_single_precision_range(value: f64) -> f64 {
    value.clamp(f32::MIN as f64, f32::MAX as f64)
}

fn round_to_nearest_integer(value: f64) -> i32 {
    if value.is_nan() {
        return 0;
    }
    (value + 0.5).floor().clamp(i32::MIN as f64, i32::MAX as f64) as i32
}

fn equals_ascii_case_insensitive(value: &[u16], expected: &[u8]) -> bool {
    value.len() == expected.len()
        && value
            .iter()
            .zip(expected)
            .all(|(&left, &right)| u8::try_from(left).is_ok_and(|left| left.eq_ignore_ascii_case(&right)))
}

fn is_css_wide_keyword(keyword: u16) -> bool {
    matches!(
        keyword,
        keyword::INHERIT | keyword::INITIAL | keyword::UNSET | keyword::REVERT | keyword::REVERT_LAYER
    )
}

fn is_valid_custom_ident(identifier: &[u16], blacklist: &[&str]) -> bool {
    if keyword_from_ascii_case_insensitive(identifier).is_some_and(is_css_wide_keyword)
        || equals_ascii_case_insensitive(identifier, b"default")
    {
        return false;
    }
    !blacklist
        .iter()
        .any(|blocked| equals_ascii_case_insensitive(identifier, blocked.as_bytes()))
}

fn retain_fly_string(context: &ParseContext, string: &[u16]) -> Option<RetainedUtf16FlyString> {
    let callback = context.intern_utf16_fly_string?;
    let raw = unsafe { callback(string.as_ptr(), string.len()) };
    Some(unsafe { RetainedUtf16FlyString::from_leaked_raw(raw) })
}

fn string_style_value(context: &ParseContext, string: &[u16]) -> Option<StyleValueData> {
    Some(StyleValueData::String {
        string: retain_fly_string(context, string)?,
        is_valid_animation_name_custom_ident: is_valid_custom_ident(string, &["none"]),
    })
}

fn parse_string_value(context: &ParseContext, value: &ComponentValue) -> Option<StyleValueData> {
    string_style_value(context, value.string()?)
}

fn parse_custom_ident_value(
    context: &ParseContext,
    value: &ComponentValue,
    blacklist: &[&str],
) -> Option<StyleValueData> {
    let identifier = value.ident()?;
    if !is_valid_custom_ident(identifier, blacklist) {
        return None;
    }
    Some(StyleValueData::CustomIdent {
        custom_ident: retain_fly_string(context, identifier)?,
    })
}

fn parse_dashed_ident_value(context: &ParseContext, value: &ComponentValue) -> Option<StyleValueData> {
    let identifier = value.ident()?;
    if !identifier.starts_with(&[u16::from(b'-'), u16::from(b'-')]) || !is_valid_custom_ident(identifier, &[]) {
        return None;
    }
    Some(StyleValueData::CustomIdent {
        custom_ident: retain_fly_string(context, identifier)?,
    })
}

fn single_modifier_argument(values: &[ComponentValue]) -> Option<&ComponentValue> {
    single_non_whitespace_value(values)
}

fn parse_url_value(context: &ParseContext, value: &ComponentValue) -> Option<StyleValueData> {
    let (url, url_type, modifiers) = match &value.kind {
        ComponentKind::Token(ParserTokenKind::Url(url)) => (url.as_ref(), 0, Vec::new()),
        ComponentKind::Function { name, values }
            if equals_ascii_case_insensitive(name, b"url") || equals_ascii_case_insensitive(name, b"src") =>
        {
            let url_type = u8::from(equals_ascii_case_insensitive(name, b"src"));
            let mut values = values.iter().filter(|value| !value.is_whitespace());
            let url = values.next()?.string()?;
            let mut modifiers = Vec::new();
            let mut seen_modifier_types = 0u8;
            for modifier in values {
                let (name, arguments) = modifier.function()?;
                let (modifier_type, modifier) = if equals_ascii_case_insensitive(name, b"cross-origin") {
                    let keyword = keyword_from_ascii_case_insensitive(single_modifier_argument(arguments)?.ident()?)?;
                    let value = keyword_to_cross_origin_modifier_value(keyword)?;
                    (0, RetainedRequestUrlModifier::from_enum(0, value))
                } else if equals_ascii_case_insensitive(name, b"integrity") {
                    let string = single_modifier_argument(arguments)?.string()?;
                    (
                        1,
                        RetainedRequestUrlModifier::from_string(1, retain_fly_string(context, string)?),
                    )
                } else if equals_ascii_case_insensitive(name, b"referrer-policy") {
                    let keyword = keyword_from_ascii_case_insensitive(single_modifier_argument(arguments)?.ident()?)?;
                    let value = keyword_to_referrer_policy_modifier_value(keyword)?;
                    (2, RetainedRequestUrlModifier::from_enum(2, value))
                } else {
                    return None;
                };
                let modifier_bit = 1 << modifier_type;
                if seen_modifier_types & modifier_bit != 0 {
                    return None;
                }
                seen_modifier_types |= modifier_bit;
                modifiers.push(modifier);
            }
            modifiers.sort_by_key(RetainedRequestUrlModifier::modifier_type);
            (url, url_type, modifiers)
        }
        _ => return None,
    };
    Some(StyleValueData::Url {
        url: RetainedString::from_utf16(url)?,
        url_type,
        modifiers: RetainedRequestUrlModifierList::from_retained_modifiers(modifiers),
    })
}

fn value_list(values: Vec<StyleValueData>, separator: u8, collapsible: bool) -> StyleValueData {
    StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(
            values.into_iter().map(RetainedStyleValueData::from_owned).collect(),
        ),
        separator,
        collapsible,
    }
}

fn unit_index(unit: &[u16], names: &[&str]) -> Option<u8> {
    names
        .iter()
        .position(|name| equals_ascii_case_insensitive(unit, name.as_bytes()))
        .and_then(|index| u8::try_from(index).ok())
}

fn number_token(value: &ComponentValue) -> Option<(f64, CssNumberType)> {
    let ComponentKind::Token(ParserTokenKind::Number { value, number_type }) = &value.kind else {
        return None;
    };
    Some((clamp_to_single_precision_range(*value), *number_type))
}

fn parse_integer_value(value: &ComponentValue, accepted_range: NumericRange) -> Option<StyleValueData> {
    let (value, number_type) = number_token(value)?;
    if number_type == CssNumberType::Number {
        return None;
    }
    let value = round_to_nearest_integer(value);
    accepted_range
        .contains(f64::from(value))
        .then_some(StyleValueData::Integer { value })
}

fn parse_number_value(value: &ComponentValue, accepted_range: NumericRange) -> Option<StyleValueData> {
    let (value, _) = number_token(value)?;
    accepted_range
        .contains(value)
        .then_some(StyleValueData::Number { value })
}

fn parse_percentage_value(value: &ComponentValue, accepted_range: NumericRange) -> Option<StyleValueData> {
    let ComponentKind::Token(ParserTokenKind::Percentage { value, .. }) = &value.kind else {
        return None;
    };
    let value = clamp_to_single_precision_range(*value);
    accepted_range
        .contains(value)
        .then_some(StyleValueData::Percentage { value })
}

fn parse_number_percentage_value(
    value: &ComponentValue,
    accepted_number_range: NumericRange,
    accepted_percentage_range: NumericRange,
) -> Option<StyleValueData> {
    parse_number_value(value, accepted_number_range)
        .or_else(|| parse_percentage_value(value, accepted_percentage_range))
}

fn parse_dimension_value(
    value: &ComponentValue,
    value_type: u8,
    accepted_range: NumericRange,
) -> Option<StyleValueData> {
    let ComponentKind::Token(ParserTokenKind::Dimension { value, unit, .. }) = &value.kind else {
        return None;
    };
    let value = clamp_to_single_precision_range(*value);
    let parse_unit = |names: &[&str], ratios: &[f64]| {
        let unit = unit_index(unit, names)?;
        accepted_range
            .contains(value * ratios[usize::from(unit)])
            .then_some(unit)
    };
    match value_type {
        VALUE_TYPE_ANGLE => parse_unit(&ANGLE_UNIT_NAMES, &ANGLE_UNIT_CANONICAL_RATIOS)
            .map(|unit| StyleValueData::Angle { value, unit }),
        VALUE_TYPE_FLEX => {
            parse_unit(&FLEX_UNIT_NAMES, &FLEX_UNIT_CANONICAL_RATIOS).map(|unit| StyleValueData::Flex { value, unit })
        }
        VALUE_TYPE_FREQUENCY => parse_unit(&FREQUENCY_UNIT_NAMES, &FREQUENCY_UNIT_CANONICAL_RATIOS)
            .map(|unit| StyleValueData::Frequency { value, unit }),
        VALUE_TYPE_LENGTH => {
            let unit = unit_index(unit, &LENGTH_UNIT_NAMES)?;
            accepted_range
                .contains(value)
                .then_some(StyleValueData::Length { value, unit })
        }
        VALUE_TYPE_RESOLUTION if value >= 0.0 => parse_unit(&RESOLUTION_UNIT_NAMES, &RESOLUTION_UNIT_CANONICAL_RATIOS)
            .map(|unit| StyleValueData::Resolution { value, unit }),
        VALUE_TYPE_TIME => {
            parse_unit(&TIME_UNIT_NAMES, &TIME_UNIT_CANONICAL_RATIOS).map(|unit| StyleValueData::Time { value, unit })
        }
        _ => None,
    }
}

fn parse_angle_value(
    context: &ParseContext,
    value: &ComponentValue,
    accepted_range: NumericRange,
) -> Option<StyleValueData> {
    parse_dimension_value(value, VALUE_TYPE_ANGLE, accepted_range).or_else(|| {
        context
            .is_svg_presentation_attribute
            .then(|| {
                let (value, _) = number_token(value)?;
                accepted_range.contains(value).then_some(StyleValueData::Angle {
                    value,
                    unit: unit_index(&['d' as u16, 'e' as u16, 'g' as u16], &ANGLE_UNIT_NAMES)?,
                })
            })
            .flatten()
    })
}

fn parse_angle_percentage_value(
    context: &ParseContext,
    value: &ComponentValue,
    accepted_angle_range: NumericRange,
    accepted_percentage_range: NumericRange,
) -> Option<StyleValueData> {
    parse_angle_value(context, value, accepted_angle_range)
        .or_else(|| parse_percentage_value(value, accepted_percentage_range))
}

fn parse_flex_value(value: &ComponentValue, accepted_range: NumericRange) -> Option<StyleValueData> {
    parse_dimension_value(value, VALUE_TYPE_FLEX, accepted_range)
}

fn parse_frequency_value(value: &ComponentValue, accepted_range: NumericRange) -> Option<StyleValueData> {
    parse_dimension_value(value, VALUE_TYPE_FREQUENCY, accepted_range)
}

fn parse_frequency_percentage_value(
    value: &ComponentValue,
    accepted_frequency_range: NumericRange,
    accepted_percentage_range: NumericRange,
) -> Option<StyleValueData> {
    parse_frequency_value(value, accepted_frequency_range)
        .or_else(|| parse_percentage_value(value, accepted_percentage_range))
}

fn parse_length_value(
    context: &ParseContext,
    property: u16,
    value: &ComponentValue,
    accepted_range: NumericRange,
) -> Option<StyleValueData> {
    parse_dimension_value(value, VALUE_TYPE_LENGTH, accepted_range).or_else(|| {
        let (value, _) = number_token(value)?;
        let allows_unitless = value == 0.0
            || context.is_svg_presentation_attribute
            || (context.in_quirks_mode && property_has_unitless_length_quirk(property));
        if !allows_unitless {
            return None;
        }
        let value = if value == 0.0 {
            0.0
        } else {
            CssPixels::nearest_value_for(value).to_double()
        };
        accepted_range.contains(value).then_some(StyleValueData::Length {
            value,
            unit: px_length_unit(),
        })
    })
}

fn parse_length_percentage_value(
    context: &ParseContext,
    property: u16,
    value: &ComponentValue,
    accepted_length_range: NumericRange,
    accepted_percentage_range: NumericRange,
) -> Option<StyleValueData> {
    parse_length_value(context, property, value, accepted_length_range)
        .or_else(|| parse_percentage_value(value, accepted_percentage_range))
}

fn parse_resolution_value(value: &ComponentValue, accepted_range: NumericRange) -> Option<StyleValueData> {
    parse_dimension_value(value, VALUE_TYPE_RESOLUTION, accepted_range)
}

fn parse_time_value(value: &ComponentValue, accepted_range: NumericRange) -> Option<StyleValueData> {
    parse_dimension_value(value, VALUE_TYPE_TIME, accepted_range)
}

fn parse_time_percentage_value(
    value: &ComponentValue,
    accepted_time_range: NumericRange,
    accepted_percentage_range: NumericRange,
) -> Option<StyleValueData> {
    parse_time_value(value, accepted_time_range).or_else(|| parse_percentage_value(value, accepted_percentage_range))
}

fn parse_ratio_value(values: &[&ComponentValue]) -> Option<StyleValueData> {
    let numerator = parse_number_value(values.first()?, NumericRange::NON_NEGATIVE)?;
    let denominator = match values {
        [_] => StyleValueData::Number { value: 1.0 },
        [_, slash, denominator] if slash.is_delim(b'/') => parse_number_value(denominator, NumericRange::NON_NEGATIVE)?,
        _ => return None,
    };
    Some(StyleValueData::Ratio {
        numerator: RetainedStyleValueData::from_owned(numerator),
        denominator: RetainedStyleValueData::from_owned(denominator),
    })
}

fn parse_opacity_value(value: &ComponentValue) -> Option<StyleValueData> {
    let value = parse_number_percentage_value(value, NumericRange::INFINITE, NumericRange::INFINITE)?;
    Some(StyleValueData::OpacityValue {
        value: RetainedStyleValueData::from_owned(value),
    })
}

fn is_math_function(name: &[u16]) -> bool {
    // NB: This is the MathFunctions.json set plus calc(). Task 006 replaces
    //     this fallback check with the generated math-function parser.
    [
        "abs", "acos", "asin", "atan", "atan2", "calc", "clamp", "cos", "exp", "hypot", "log", "max", "min", "mod",
        "pow", "progress", "random", "rem", "round", "sign", "sin", "sqrt", "tan",
    ]
    .iter()
    .any(|function| equals_ascii_case_insensitive(name, function.as_bytes()))
}

fn is_arbitrary_substitution_function(name: &[u16]) -> bool {
    (name.len() >= 2 && name[0] == u16::from(b'-') && name[1] == u16::from(b'-'))
        || ["attr", "env", "if", "inherit", "var"]
            .iter()
            .any(|function| equals_ascii_case_insensitive(name, function.as_bytes()))
}

fn unported_function_reason(values: &[ComponentValue]) -> Option<&'static NotHandledReason> {
    values.iter().find_map(|value| match &value.kind {
        ComponentKind::Function { name, values } => {
            if is_arbitrary_substitution_function(name) {
                Some(&SUBSTITUTION_NOT_PORTED)
            } else if is_math_function(name) {
                Some(&CALC_NOT_PORTED)
            } else if equals_ascii_case_insensitive(name, b"sibling-count")
                || equals_ascii_case_insensitive(name, b"sibling-index")
            {
                Some(&FUNCTION_NOT_PORTED)
            } else {
                unported_function_reason(values)
            }
        }
        ComponentKind::SimpleBlock { values, .. } => unported_function_reason(values),
        ComponentKind::Token(_) => None,
    })
}

fn property_uses_numeric_parser(property: u16) -> bool {
    property_accepted_value_types(property)
        .iter()
        .any(|value_type| PORTED_NUMERIC_VALUE_TYPES.contains(value_type))
}

fn property_numeric_grammar_is_fully_ported(property: u16) -> bool {
    let accepted_types = property_accepted_value_types(property);
    !accepted_types.is_empty()
        && accepted_types
            .iter()
            .all(|value_type| PORTED_NUMERIC_VALUE_TYPES.contains(value_type))
}

fn property_leaf_grammar_is_fully_ported(property: u16) -> bool {
    let accepted_types = property_accepted_value_types(property);
    !accepted_types.is_empty()
        && accepted_types.iter().all(|value_type| {
            PORTED_NUMERIC_VALUE_TYPES.contains(value_type) || PORTED_TEXT_VALUE_TYPES.contains(value_type)
        })
}

fn parse_single_numeric_value_type(
    context: &ParseContext,
    property: u16,
    value_type: u8,
    value: &ComponentValue,
) -> Option<StyleValueData> {
    match value_type {
        VALUE_TYPE_OPACITY_VALUE => parse_opacity_value(value),
        VALUE_TYPE_INTEGER => parse_integer_value(value, accepted_range(property, VALUE_TYPE_INTEGER)),
        VALUE_TYPE_NUMBER => parse_number_value(value, accepted_range(property, VALUE_TYPE_NUMBER)),
        VALUE_TYPE_ANGLE => {
            if property_percentages_resolve_to(property) == Some(VALUE_TYPE_ANGLE) {
                parse_angle_percentage_value(
                    context,
                    value,
                    accepted_range(property, VALUE_TYPE_ANGLE),
                    accepted_range(property, VALUE_TYPE_PERCENTAGE),
                )
            } else {
                parse_angle_value(context, value, accepted_range(property, VALUE_TYPE_ANGLE))
            }
        }
        VALUE_TYPE_FLEX => parse_flex_value(value, accepted_range(property, VALUE_TYPE_FLEX)),
        VALUE_TYPE_FREQUENCY => {
            if property_percentages_resolve_to(property) == Some(VALUE_TYPE_FREQUENCY) {
                parse_frequency_percentage_value(
                    value,
                    accepted_range(property, VALUE_TYPE_FREQUENCY),
                    accepted_range(property, VALUE_TYPE_PERCENTAGE),
                )
            } else {
                parse_frequency_value(value, accepted_range(property, VALUE_TYPE_FREQUENCY))
            }
        }
        VALUE_TYPE_LENGTH => {
            if property_percentages_resolve_to(property) == Some(VALUE_TYPE_LENGTH) {
                parse_length_percentage_value(
                    context,
                    property,
                    value,
                    accepted_range(property, VALUE_TYPE_LENGTH),
                    accepted_range(property, VALUE_TYPE_PERCENTAGE),
                )
            } else {
                parse_length_value(context, property, value, accepted_range(property, VALUE_TYPE_LENGTH))
            }
        }
        VALUE_TYPE_RESOLUTION => parse_resolution_value(value, accepted_range(property, VALUE_TYPE_RESOLUTION)),
        VALUE_TYPE_TIME => {
            if property_percentages_resolve_to(property) == Some(VALUE_TYPE_TIME) {
                parse_time_percentage_value(
                    value,
                    accepted_range(property, VALUE_TYPE_TIME),
                    accepted_range(property, VALUE_TYPE_PERCENTAGE),
                )
            } else {
                parse_time_value(value, accepted_range(property, VALUE_TYPE_TIME))
            }
        }
        VALUE_TYPE_PERCENTAGE => parse_percentage_value(value, accepted_range(property, VALUE_TYPE_PERCENTAGE)),
        _ => None,
    }
}

fn parse_single_text_value_type(
    context: &ParseContext,
    property: u16,
    value_type: u8,
    value: &ComponentValue,
) -> Option<StyleValueData> {
    match value_type {
        VALUE_TYPE_CUSTOM_IDENT => parse_custom_ident_value(context, value, property_custom_ident_blacklist(property)),
        VALUE_TYPE_DASHED_IDENT => parse_dashed_ident_value(context, value),
        VALUE_TYPE_STRING => parse_string_value(context, value),
        // NB: C++ tries <image> before <url>. A non-fragment url() accepted as
        //     an image captures the stylesheet base URL in ImageStyleValue.
        VALUE_TYPE_URL if !property_accepted_value_types(property).contains(&VALUE_TYPE_IMAGE) => {
            parse_url_value(context, value)
        }
        _ => None,
    }
}

fn parse_property_keyword_data(property: u16, value: &ComponentValue) -> Option<StyleValueData> {
    let keyword = keyword_from_ascii_case_insensitive(value.ident()?)?;
    property_accepted_keywords(property)
        .binary_search(&keyword)
        .is_ok()
        .then(|| StyleValueData::Keyword {
            keyword: property_resolve_legacy_value_alias(property, keyword),
        })
}

fn parse_single_property_leaf(context: &ParseContext, property: u16, value: &ComponentValue) -> Option<StyleValueData> {
    if let Some(keyword) = parse_property_keyword_data(property, value) {
        return Some(keyword);
    }

    let accepted_types = property_accepted_value_types(property);
    if accepted_types.contains(&VALUE_TYPE_CUSTOM_IDENT)
        && let Some(parsed) = parse_single_text_value_type(context, property, VALUE_TYPE_CUSTOM_IDENT, value)
    {
        return Some(parsed);
    }
    for value_type in [VALUE_TYPE_DASHED_IDENT, VALUE_TYPE_STRING, VALUE_TYPE_URL] {
        if accepted_types.contains(&value_type)
            && let Some(parsed) = parse_single_text_value_type(context, property, value_type, value)
        {
            return Some(parsed);
        }
    }
    for value_type in [
        VALUE_TYPE_OPACITY_VALUE,
        VALUE_TYPE_INTEGER,
        VALUE_TYPE_NUMBER,
        VALUE_TYPE_ANGLE,
        VALUE_TYPE_FLEX,
        VALUE_TYPE_FREQUENCY,
        VALUE_TYPE_LENGTH,
        VALUE_TYPE_RESOLUTION,
        VALUE_TYPE_TIME,
        VALUE_TYPE_PERCENTAGE,
    ] {
        if accepted_types.contains(&value_type)
            && let Some(parsed) = parse_single_numeric_value_type(context, property, value_type, value)
        {
            return Some(parsed);
        }
    }
    None
}

fn parse_specific_keyword(value: &ComponentValue, accepted: &[u16]) -> Option<StyleValueData> {
    let keyword = keyword_from_ascii_case_insensitive(value.ident()?)?;
    accepted
        .contains(&keyword)
        .then_some(StyleValueData::Keyword { keyword })
}

fn parse_comma_separated_dashed_ident_list(
    context: &ParseContext,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let values = values
        .split(ComponentValue::is_comma)
        .map(|item| parse_dashed_ident_value(context, single_non_whitespace_value(item)?))
        .collect::<Option<Vec<_>>>()?;
    Some(value_list(values, 1, true))
}

fn parse_special_text_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if !matches!(
        property,
        property_id::ANCHOR_NAME
            | property_id::ANCHOR_SCOPE
            | property_id::CONTAINER_NAME
            | property_id::FONT_LANGUAGE_OVERRIDE
            | property_id::POSITION_ANCHOR
            | property_id::TIMELINE_SCOPE
            | property_id::TRANSITION_PROPERTY
            | property_id::WILL_CHANGE
    ) {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    }
    if let Some(reason) = unported_function_reason(values) {
        return ParseOutcome::NotHandled(reason);
    }

    let single_value = single_non_whitespace_value(values);
    let parsed = match property {
        property_id::ANCHOR_NAME => single_value
            .and_then(|value| parse_specific_keyword(value, &[keyword::NONE]))
            .or_else(|| parse_comma_separated_dashed_ident_list(context, values)),
        property_id::ANCHOR_SCOPE | property_id::TIMELINE_SCOPE => single_value
            .and_then(|value| parse_specific_keyword(value, &[keyword::NONE, keyword::ALL]))
            .or_else(|| parse_comma_separated_dashed_ident_list(context, values)),
        property_id::POSITION_ANCHOR => single_value.and_then(|value| {
            parse_specific_keyword(value, &[keyword::NORMAL, keyword::NONE, keyword::AUTO])
                .or_else(|| parse_dashed_ident_value(context, value))
        }),
        property_id::CONTAINER_NAME => single_value
            .and_then(|value| parse_specific_keyword(value, &[keyword::NONE]))
            .or_else(|| {
                let names = values
                    .iter()
                    .filter(|value| !value.is_whitespace())
                    .map(|value| parse_custom_ident_value(context, value, &["none", "and", "not", "or"]))
                    .collect::<Option<Vec<_>>>()?;
                match names.as_slice() {
                    [] => None,
                    [_] => names.into_iter().next(),
                    _ => Some(value_list(names, 0, false)),
                }
            }),
        property_id::FONT_LANGUAGE_OVERRIDE => single_value.and_then(|value| {
            if let Some(normal) = parse_specific_keyword(value, &[keyword::NORMAL]) {
                return Some(normal);
            }
            let string = value.string()?;
            if string.is_empty() || string.len() > 4 || !string.iter().all(|&code_unit| code_unit <= 0x7f) {
                return None;
            }
            let trimmed_length = string
                .iter()
                .rposition(|code_unit| !matches!(*code_unit, 0x09..=0x0d | 0x20))
                .map(|index| index + 1)?;
            string_style_value(context, &string[..trimmed_length])
        }),
        property_id::TRANSITION_PROPERTY => {
            if let Some(none) = single_value.and_then(|value| parse_specific_keyword(value, &[keyword::NONE])) {
                Some(value_list(vec![none], 1, true))
            } else {
                (|| {
                    let properties = values
                        .split(ComponentValue::is_comma)
                        .map(|item| parse_custom_ident_value(context, single_non_whitespace_value(item)?, &["none"]))
                        .collect::<Option<Vec<_>>>()?;
                    Some(value_list(properties, 1, true))
                })()
            }
        }
        property_id::WILL_CHANGE => {
            if let Some(auto) = single_value.and_then(|value| parse_specific_keyword(value, &[keyword::AUTO])) {
                Some(auto)
            } else {
                (|| {
                    let features = values
                        .split(ComponentValue::is_comma)
                        .map(|item| {
                            let value = single_non_whitespace_value(item)?;
                            parse_specific_keyword(value, &[keyword::SCROLL_POSITION, keyword::CONTENTS]).or_else(
                                || {
                                    parse_custom_ident_value(
                                        context,
                                        value,
                                        &["all", "auto", "contents", "none", "scroll-position", "will-change"],
                                    )
                                },
                            )
                        })
                        .collect::<Option<Vec<_>>>()?;
                    Some(value_list(features, 1, true))
                })()
            }
        }
        _ => unreachable!(),
    };
    parsed.map_or(ParseOutcome::Invalid, |parsed| ParseOutcome::Parsed(Arc::new(parsed)))
}

fn parse_coordinating_value_list(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if !(FIRST_SHORTHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property)
        || property_is_shorthand(property)
        || !property_has_coordinating_list_multiplicity(property)
        || property_uses_special_keyword_parser(property)
    {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    }
    if let Some(reason) = unported_function_reason(values) {
        return ParseOutcome::NotHandled(reason);
    }

    let fully_ported = property_leaf_grammar_is_fully_ported(property);
    let mut parsed_values = Vec::new();
    for item in values.split(ComponentValue::is_comma) {
        let mut item_values = item.iter().filter(|value| !value.is_whitespace());
        let Some(value) = item_values.next() else {
            return if fully_ported {
                ParseOutcome::Invalid
            } else {
                ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED)
            };
        };
        if item_values.next().is_some() {
            // NB: C++ numeric leaves can reach the calculation parser for
            //     operator expressions without an explicit calc() wrapper.
            return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
        }
        let Some(parsed) = parse_single_property_leaf(context, property, value) else {
            return if fully_ported {
                ParseOutcome::Invalid
            } else {
                ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED)
            };
        };
        parsed_values.push(parsed);
    }
    ParseOutcome::Parsed(Arc::new(value_list(parsed_values, 1, true)))
}

fn parse_generic_text_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if !(FIRST_SHORTHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property)
        || property_is_shorthand(property)
        || property_has_coordinating_list_multiplicity(property)
        || property_uses_special_keyword_parser(property)
        || !property_accepted_value_types(property)
            .iter()
            .any(|value_type| PORTED_TEXT_VALUE_TYPES.contains(value_type))
    {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    }
    if let Some(reason) = unported_function_reason(values) {
        return ParseOutcome::NotHandled(reason);
    }

    let Some(value) = single_non_whitespace_value(values) else {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    };
    if let Some(parsed) = parse_single_property_leaf(context, property, value) {
        return ParseOutcome::Parsed(Arc::new(parsed));
    }
    if property_leaf_grammar_is_fully_ported(property) {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED)
}

fn parse_generic_numeric_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if !(FIRST_SHORTHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property)
        || property_is_shorthand(property)
        || property_has_coordinating_list_multiplicity(property)
        || property_uses_special_keyword_parser(property)
        || !property_uses_numeric_parser(property)
    {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    }
    if let Some(reason) = unported_function_reason(values) {
        return ParseOutcome::NotHandled(reason);
    }

    let non_whitespace_values = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    let accepted_types = property_accepted_value_types(property);
    let single_value = (non_whitespace_values.len() == 1).then(|| non_whitespace_values[0]);
    let mut parsed = accepted_types
        .contains(&VALUE_TYPE_RATIO)
        .then(|| parse_ratio_value(&non_whitespace_values))
        .flatten();
    if let Some(value) = single_value {
        // NB: Keep this in the same order as parse_css_value_for_properties() in
        //     PropertyParsing.cpp. Each accepted grammar is attempted even when
        //     an earlier one accepted by the property does not match this token.
        for value_type in [
            VALUE_TYPE_OPACITY_VALUE,
            VALUE_TYPE_INTEGER,
            VALUE_TYPE_NUMBER,
            VALUE_TYPE_ANGLE,
            VALUE_TYPE_FLEX,
            VALUE_TYPE_FREQUENCY,
            VALUE_TYPE_LENGTH,
            VALUE_TYPE_RESOLUTION,
            VALUE_TYPE_TIME,
            VALUE_TYPE_PERCENTAGE,
        ] {
            if parsed.is_none() && accepted_types.contains(&value_type) {
                parsed = parse_single_numeric_value_type(context, property, value_type, value);
            }
        }
    }

    if let Some(parsed) = parsed {
        return ParseOutcome::Parsed(Arc::new(parsed));
    }
    if property_numeric_grammar_is_fully_ported(property) && single_value.is_some() {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED)
}

fn parse_generic_property_keyword(property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if !(FIRST_SHORTHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property)
        || property_is_shorthand(property)
        || property_has_coordinating_list_multiplicity(property)
        || property_uses_special_keyword_parser(property)
    {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    }

    let Some(identifier) = single_non_whitespace_value(values).and_then(ComponentValue::ident) else {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    };
    let parsed_keyword = keyword_from_ascii_case_insensitive(identifier);
    if let Some(parsed_keyword) = parsed_keyword
        && property_accepted_keywords(property)
            .binary_search(&parsed_keyword)
            .is_ok()
    {
        let keyword = property_resolve_legacy_value_alias(property, parsed_keyword);
        return ParseOutcome::Parsed(Arc::new(StyleValueData::Keyword { keyword }));
    }
    if property_accepts_only_keywords(property) {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED)
}

fn parse_display_keyword(values: &[ComponentValue]) -> ParseOutcome {
    let Some(identifier) = single_non_whitespace_value(values).and_then(ComponentValue::ident) else {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    };
    let Some(keyword) = keyword_from_ascii_case_insensitive(identifier) else {
        return ParseOutcome::Invalid;
    };
    let Some(display) = FfiDisplay::from_single_keyword(keyword) else {
        return ParseOutcome::Invalid;
    };
    ParseOutcome::Parsed(Arc::new(StyleValueData::Display { raw: display.encoded() }))
}

/// Parse a property value using the grammars which have been ported to Rust.
///
/// `Invalid` is reserved for grammars which Rust handles completely. Until a
/// grammar is ported, C++ remains authoritative through `NotHandled`.
pub(crate) fn parse_css_value(context: &ParseContext, property_id: u16, values: &[ComponentValue]) -> ParseOutcome {
    if let Some(value) = parse_builtin_value(values) {
        return ParseOutcome::Parsed(Arc::new(value));
    }
    if let Some(reason) = unported_function_reason(values) {
        return ParseOutcome::NotHandled(reason);
    }
    if property_id == property_id::DISPLAY {
        return parse_display_keyword(values);
    }
    let special_text_outcome = parse_special_text_property(context, property_id, values);
    if !matches!(special_text_outcome, ParseOutcome::NotHandled(_)) {
        return special_text_outcome;
    }
    let coordinating_list_outcome = parse_coordinating_value_list(context, property_id, values);
    if !matches!(coordinating_list_outcome, ParseOutcome::NotHandled(_)) {
        return coordinating_list_outcome;
    }
    let keyword_outcome = parse_generic_property_keyword(property_id, values);
    if !matches!(keyword_outcome, ParseOutcome::NotHandled(_)) {
        return keyword_outcome;
    }
    let text_outcome = parse_generic_text_property(context, property_id, values);
    if !matches!(text_outcome, ParseOutcome::NotHandled(_)) {
        return text_outcome;
    }
    parse_generic_numeric_property(context, property_id, values)
}

/// The result category returned through the value-parser FFI.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FfiParseStatus {
    Parsed,
    Invalid,
    NotHandled,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum StatisticKind {
    Parsed,
    NotHandled,
}

type StatisticKey = (StatisticKind, u16, &'static str);

fn statistics() -> &'static Mutex<BTreeMap<StatisticKey, u64>> {
    static STATISTICS: OnceLock<Mutex<BTreeMap<StatisticKey, u64>>> = OnceLock::new();
    STATISTICS.get_or_init(|| Mutex::new(BTreeMap::new()))
}

fn statistics_enabled() -> bool {
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var("LIBWEB_PARSE_FALLBACK_STATS").as_deref() == Ok("1"))
}

fn record_outcome(property_id: u16, outcome: &ParseOutcome) {
    if !statistics_enabled() {
        return;
    }
    let key = match outcome {
        ParseOutcome::Parsed(_) => (StatisticKind::Parsed, property_id, ""),
        ParseOutcome::Invalid => return,
        ParseOutcome::NotHandled(reason) => (StatisticKind::NotHandled, property_id, reason.label),
    };
    let mut statistics = statistics().lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    *statistics.entry(key).or_default() += 1;
}

/// Tries the Rust value parser and returns a strong StyleValueData reference
/// on success. A null return is disambiguated by `out_status`.
///
/// # Safety
/// All non-null pointers must remain readable for their accompanying lengths
/// for the duration of this call. `out_status` and `out_reason` must be valid
/// writable pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_css_value(
    context: *const ParseContext,
    property_id: u16,
    source: FfiUtf16View,
    out_status: *mut FfiParseStatus,
    out_reason: *mut *const u8,
) -> *const c_void {
    crate::abort_on_panic(|| {
        let invalid_ffi_result = || {
            if !out_status.is_null() {
                unsafe { *out_status = FfiParseStatus::NotHandled };
            }
            if !out_reason.is_null() {
                unsafe { *out_reason = INVALID_FFI_INPUT.c_label.as_ptr() };
            }
            std::ptr::null()
        };

        if context.is_null() || out_status.is_null() || out_reason.is_null() {
            return invalid_ffi_result();
        }
        let Some(source) = (unsafe { source.units() }) else {
            return invalid_ffi_result();
        };
        let context = unsafe { &*context };
        let outcome = match consume_a_list_of_component_values(&tokenize_for_parser(source)) {
            Ok(values) => parse_css_value(context, property_id, &values),
            Err(()) => ParseOutcome::NotHandled(&COMPONENT_VALUES_INVALID),
        };
        record_outcome(property_id, &outcome);

        match outcome {
            ParseOutcome::Parsed(value) => {
                unsafe {
                    *out_status = FfiParseStatus::Parsed;
                    *out_reason = std::ptr::null();
                }
                Arc::into_raw(value).cast()
            }
            ParseOutcome::Invalid => {
                unsafe {
                    *out_status = FfiParseStatus::Invalid;
                    *out_reason = std::ptr::null();
                }
                std::ptr::null()
            }
            ParseOutcome::NotHandled(reason) => {
                unsafe {
                    *out_status = FfiParseStatus::NotHandled;
                    *out_reason = reason.c_label.as_ptr();
                }
                std::ptr::null()
            }
        }
    })
}

/// Prints the per-property Rust parse and C++ fallback counts accumulated by
/// this process. C++ calls this at process exit when statistics are enabled.
#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_fallback_stats_dump() {
    if !statistics_enabled() {
        return;
    }
    let statistics = statistics().lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    for (&(kind, property_id, reason), &count) in statistics.iter() {
        let property_name = crate::css::property_metadata::property_name(property_id);
        match kind {
            StatisticKind::Parsed => {
                eprintln!("LIBWEB_PARSE_VALUE parsed property={property_name} property-id={property_id} count={count}");
            }
            StatisticKind::NotHandled => {
                eprintln!(
                    "LIBWEB_PARSE_VALUE not-handled property={property_name} property-id={property_id} reason={reason} count={count}"
                );
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, property_initial_value, property_name};

    unsafe extern "C" fn discard_interned_string(_: *const u16, _: usize) -> usize {
        0
    }

    fn context() -> ParseContext {
        ParseContext {
            in_quirks_mode: false,
            is_svg_presentation_attribute: false,
            value_contexts: std::ptr::null(),
            value_context_count: 0,
            document_url: std::ptr::null(),
            document_url_length: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            intern_utf16_fly_string: Some(discard_interned_string),
        }
    }

    fn parse(property: u16, source: &str) -> ParseOutcome {
        let values = consume_a_list_of_component_values(&tokenize_for_parser(source.as_bytes())).unwrap();
        parse_css_value(&context(), property, &values)
    }

    fn parse_with_context(context: &ParseContext, property: u16, source: &str) -> ParseOutcome {
        let values = consume_a_list_of_component_values(&tokenize_for_parser(source.as_bytes())).unwrap();
        parse_css_value(context, property, &values)
    }

    fn component(source: &str) -> ComponentValue {
        let mut values = consume_a_list_of_component_values(&tokenize_for_parser(source.as_bytes())).unwrap();
        assert_eq!(values.len(), 1);
        values.remove(0)
    }

    fn parsed_keyword(outcome: ParseOutcome) -> u16 {
        let ParseOutcome::Parsed(value) = outcome else {
            panic!("value should parse");
        };
        let StyleValueData::Keyword { keyword } = &*value else {
            panic!("value should be a keyword");
        };
        *keyword
    }

    #[test]
    fn unported_property_falls_back_to_cpp() {
        let values = consume_a_list_of_component_values(&tokenize_for_parser(b"0.5")).unwrap();
        let ParseOutcome::NotHandled(reason) = parse_css_value(&context(), 1, &values) else {
            panic!("unported value should not be authoritative");
        };
        assert_eq!(reason.label, "property:not-ported");
    }

    #[test]
    fn parses_keywords_accepted_by_generic_properties() {
        assert_eq!(parsed_keyword(parse(property_id::APPEARANCE, "none")), keyword::NONE);
        assert_eq!(parsed_keyword(parse(property_id::WIDTH, "auto")), keyword::AUTO);
        assert_eq!(parsed_keyword(parse(property_id::OVERFLOW_X, "overlay")), keyword::AUTO);
    }

    #[test]
    fn rejects_unknown_identifiers_for_keyword_only_properties() {
        assert!(matches!(parse(property_id::APPEARANCE, "bogus"), ParseOutcome::Invalid));
    }

    #[test]
    fn leaves_mixed_and_special_grammars_with_cpp() {
        assert!(matches!(parse(property_id::COLOR, "red"), ParseOutcome::NotHandled(_)));
        assert!(matches!(
            parse(property_id::ALIGN_ITEMS, "normal"),
            ParseOutcome::NotHandled(_)
        ));
        assert!(matches!(
            parse(property_id::FONT_FAMILY, "\"Ladybird Sans\""),
            ParseOutcome::NotHandled(_)
        ));
    }

    #[test]
    fn parses_string_and_identifier_leaves() {
        assert!(matches!(
            &*match parse(property_id::LIST_STYLE_TYPE, "\"stars\"") {
                ParseOutcome::Parsed(value) => value,
                _ => panic!("string should parse"),
            },
            StyleValueData::String { .. }
        ));
        assert!(matches!(
            &*match parse(property_id::VIEW_TRANSITION_NAME, "card") {
                ParseOutcome::Parsed(value) => value,
                _ => panic!("custom-ident should parse"),
            },
            StyleValueData::CustomIdent { .. }
        ));
        assert!(matches!(
            parse(property_id::VIEW_TRANSITION_NAME, "auto"),
            ParseOutcome::Invalid
        ));
        assert!(!is_valid_custom_ident(
            &"inherit".encode_utf16().collect::<Vec<_>>(),
            &[]
        ));
        assert!(!is_valid_custom_ident(
            &"DeFaUlT".encode_utf16().collect::<Vec<_>>(),
            &[]
        ));
    }

    #[test]
    fn parses_comma_separated_leaf_lists() {
        for (property, source) in [
            (property_id::ANIMATION_NAME, "fade, \"123fade\""),
            (property_id::SCROLL_TIMELINE_NAME, "--main, none"),
            (property_id::ANIMATION_DIRECTION, "normal, reverse"),
        ] {
            let ParseOutcome::Parsed(value) = parse(property, source) else {
                panic!("coordinating list should parse: {source}");
            };
            let StyleValueData::ValueList { values, separator, .. } = &*value else {
                panic!("coordinating values should use a list");
            };
            assert_eq!(values.as_slice().len(), 2);
            assert_eq!(*separator, 1);
        }
        assert!(matches!(
            parse(property_id::ANIMATION_NAME, "fade,"),
            ParseOutcome::Invalid
        ));
        assert!(matches!(
            parse(property_id::SCROLL_TIMELINE_NAME, "ordinary-name"),
            ParseOutcome::Invalid
        ));
        assert!(matches!(
            parse(property_id::TRANSITION_DURATION, "10s + 20s"),
            ParseOutcome::NotHandled(_)
        ));
    }

    #[test]
    fn parses_url_tokens_functions_and_modifiers() {
        let ParseOutcome::Parsed(value) = parse(property_id::CLIP_PATH, "url(images/mask.svg#shape)") else {
            panic!("URL token should parse");
        };
        assert!(
            matches!(&*value, StyleValueData::Url { url, url_type: 0, .. } if url.as_bytes() == b"images/mask.svg#shape")
        );

        let value = component(
            "src(\"font.woff2\" referrer-policy(no-referrer) integrity(\"sha256-value\") cross-origin(anonymous))",
        );
        let StyleValueData::Url {
            url,
            url_type,
            modifiers,
        } = parse_url_value(&context(), &value).expect("src() should parse")
        else {
            panic!("src() should produce a URL");
        };
        assert_eq!(url.as_bytes(), b"font.woff2");
        assert_eq!(url_type, 1);
        assert_eq!(
            modifiers
                .as_slice()
                .iter()
                .map(RetainedRequestUrlModifier::modifier_type)
                .collect::<Vec<_>>(),
            [0, 1, 2]
        );

        let duplicate = component("url(\"a\" integrity(\"one\") integrity(\"two\"))");
        assert!(parse_url_value(&context(), &duplicate).is_none());
        assert!(matches!(
            parse(property_id::MASK_IMAGE, "url(mask.png)"),
            ParseOutcome::NotHandled(_)
        ));
    }

    #[test]
    fn parses_special_identifier_lists() {
        for (property, source, expected_length, expected_separator) in [
            (property_id::ANCHOR_NAME, "--main, --fallback", 2, 1),
            (property_id::ANCHOR_SCOPE, "--main", 1, 1),
            (property_id::TIMELINE_SCOPE, "--scroll, --view", 2, 1),
            (property_id::CONTAINER_NAME, "card inline-card", 2, 0),
            (property_id::TRANSITION_PROPERTY, "opacity, transform", 2, 1),
            (property_id::WILL_CHANGE, "scroll-position, opacity", 2, 1),
        ] {
            let ParseOutcome::Parsed(value) = parse(property, source) else {
                panic!("identifier list should parse: {source}");
            };
            let StyleValueData::ValueList { values, separator, .. } = &*value else {
                panic!("multiple identifiers should produce a list: {source}");
            };
            assert_eq!(values.as_slice().len(), expected_length);
            assert_eq!(*separator, expected_separator);
        }

        assert!(matches!(
            parse(property_id::ANCHOR_SCOPE, "all"),
            ParseOutcome::Parsed(_)
        ));
        assert!(matches!(
            parse(property_id::POSITION_ANCHOR, "--main"),
            ParseOutcome::Parsed(_)
        ));
        assert!(matches!(
            parse(property_id::CONTAINER_NAME, "single"),
            ParseOutcome::Parsed(_)
        ));
        assert!(matches!(
            parse(property_id::WILL_CHANGE, "auto"),
            ParseOutcome::Parsed(_)
        ));

        for (property, source) in [
            (property_id::ANCHOR_NAME, "ordinary"),
            (property_id::CONTAINER_NAME, "card and"),
            (property_id::TRANSITION_PROPERTY, "opacity,"),
            (property_id::WILL_CHANGE, "all"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
        let ParseOutcome::NotHandled(reason) = parse(property_id::ANCHOR_NAME, "var(--anchor)") else {
            panic!("substitution should remain with C++");
        };
        assert_eq!(reason.label, "substitution");
    }

    #[test]
    fn parses_font_language_override_strings() {
        for source in ["normal", "\"ENG\"", "\"ENG \""] {
            assert!(matches!(
                parse(property_id::FONT_LANGUAGE_OVERRIDE, source),
                ParseOutcome::Parsed(_)
            ));
        }
        for source in ["\"\"", "\"     \"", "\"abcde\"", "\"é\""] {
            assert!(matches!(
                parse(property_id::FONT_LANGUAGE_OVERRIDE, source),
                ParseOutcome::Invalid
            ));
        }
    }

    #[test]
    fn parses_single_keyword_display_values() {
        let ParseOutcome::Parsed(value) = parse(property_id::DISPLAY, "none") else {
            panic!("display keyword should parse");
        };
        assert!(matches!(&*value, StyleValueData::Display { raw } if *raw == FfiDisplay::none().encoded()));
        assert!(matches!(parse(property_id::DISPLAY, "bogus"), ParseOutcome::Invalid));
        assert!(matches!(
            parse(property_id::DISPLAY, "inline flow"),
            ParseOutcome::NotHandled(_)
        ));
    }

    #[test]
    fn parses_numeric_property_leaves() {
        assert!(matches!(
            &*match parse(property_id::Z_INDEX, "42") {
                ParseOutcome::Parsed(value) => value,
                _ => panic!("integer should parse"),
            },
            StyleValueData::Integer { value: 42 }
        ));
        assert!(matches!(parse(property_id::Z_INDEX, "42.0"), ParseOutcome::Invalid));

        assert!(matches!(
            &*match parse(property_id::FONT_WEIGHT, "450") {
                ParseOutcome::Parsed(value) => value,
                _ => panic!("number should parse"),
            },
            StyleValueData::Number { value } if *value == 450.0
        ));
        assert!(matches!(
            &*match parse(property_id::WIDTH, "12PX") {
                ParseOutcome::Parsed(value) => value,
                _ => panic!("length should parse"),
            },
            StyleValueData::Length { value, unit } if *value == 12.0 && *unit == px_length_unit()
        ));
        assert!(matches!(
            &*match parse(property_id::WIDTH, "25%") {
                ParseOutcome::Parsed(value) => value,
                _ => panic!("percentage should parse"),
            },
            StyleValueData::Percentage { value } if *value == 25.0
        ));
        // Width also accepts fit-content(), which is deliberately left for a
        // later task, so rejected numeric leaves cannot yet be authoritative.
        assert!(matches!(parse(property_id::WIDTH, "-1px"), ParseOutcome::NotHandled(_)));
    }

    #[test]
    fn tries_every_numeric_type_accepted_by_a_property() {
        assert!(matches!(
            &*match parse(property_id::STROKE_DASHOFFSET, "0px") {
                ParseOutcome::Parsed(value) => value,
                _ => panic!("stroke-dashoffset length should parse"),
            },
            StyleValueData::Length { value: 0.0, .. }
        ));
        assert!(matches!(
            &*match parse(property_id::STROKE_WIDTH, "1") {
                ParseOutcome::Parsed(value) => value,
                _ => panic!("stroke-width number should parse"),
            },
            StyleValueData::Number { value: 1.0 }
        ));
        assert!(matches!(parse(property_id::OPACITY, "0.5"), ParseOutcome::Parsed(_)));
        assert!(matches!(parse(property_id::Z_INDEX, "3"), ParseOutcome::Parsed(_)));
        assert!(matches!(
            &*match parse(property_id::LINE_HEIGHT, "1.2") {
                ParseOutcome::Parsed(value) => value,
                _ => panic!("line-height number should parse"),
            },
            StyleValueData::Number { value: 1.2 }
        ));
    }

    #[test]
    fn no_longhand_initial_value_is_authoritatively_rejected() {
        for property in FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID {
            let initial_value = property_initial_value(property);
            assert!(
                !matches!(parse(property, initial_value), ParseOutcome::Invalid),
                "Rust parser rejected the C++ initial value for {}: {initial_value}",
                property_name(property)
            );
        }
    }

    #[test]
    fn wraps_opacity_values() {
        for (source, is_percentage) in [("0.25", false), ("25%", true)] {
            let ParseOutcome::Parsed(value) = parse(property_id::OPACITY, source) else {
                panic!("opacity should parse");
            };
            let StyleValueData::OpacityValue { value } = &*value else {
                panic!("opacity should use its specialized wrapper");
            };
            assert_eq!(matches!(value.data(), StyleValueData::Percentage { .. }), is_percentage);
        }
    }

    #[test]
    fn mirrors_unitless_length_contexts() {
        assert!(matches!(parse(property_id::MARGIN_TOP, "12"), ParseOutcome::Invalid));

        let mut quirks_context = context();
        quirks_context.in_quirks_mode = true;
        let ParseOutcome::Parsed(value) = parse_with_context(&quirks_context, property_id::MARGIN_TOP, "12.1") else {
            panic!("quirky length should parse");
        };
        assert!(matches!(
            &*value,
            StyleValueData::Length { value, unit }
                if *value == CssPixels::nearest_value_for(12.1).to_double() && *unit == px_length_unit()
        ));

        let mut svg_context = context();
        svg_context.is_svg_presentation_attribute = true;
        assert!(
            parse_length_value(
                &svg_context,
                property_id::WIDTH,
                &component("3"),
                NumericRange::INFINITE,
            )
            .is_some()
        );
        assert!(parse_angle_value(&svg_context, &component("3"), NumericRange::INFINITE).is_some());
        assert!(parse_angle_value(&context(), &component("0"), NumericRange::INFINITE).is_none());
    }

    #[test]
    fn parses_dimension_units_and_checks_canonical_ranges() {
        let exact = |value| NumericRange { min: value, max: value };
        assert!(parse_angle_value(&context(), &component("0.5turn"), exact(180.0)).is_some());
        assert!(parse_flex_value(&component("2fr"), exact(2.0)).is_some());
        assert!(parse_frequency_value(&component("0.5kHz"), exact(500.0)).is_some());
        assert!(parse_time_value(&component("1000ms"), exact(1.0)).is_some());
        assert!(parse_resolution_value(&component("96dpi"), NumericRange { min: 0.99, max: 1.01 },).is_some());
        assert!(parse_resolution_value(&component("-1dppx"), NumericRange::INFINITE).is_none());
    }

    #[test]
    fn parses_ratio_leaves() {
        let values = consume_a_list_of_component_values(&tokenize_for_parser(b"16 / 9"))
            .unwrap()
            .into_iter()
            .filter(|value| !value.is_whitespace())
            .collect::<Vec<_>>();
        let references = values.iter().collect::<Vec<_>>();
        let StyleValueData::Ratio { numerator, denominator } = parse_ratio_value(&references).unwrap() else {
            panic!("ratio should use its specialized wrapper");
        };
        assert!(matches!(numerator.data(), StyleValueData::Number { value } if *value == 16.0));
        assert!(matches!(denominator.data(), StyleValueData::Number { value } if *value == 9.0));
        assert!(parse_ratio_value(&[&component("-1")]).is_none());
    }

    #[test]
    fn leaves_calculations_with_cpp() {
        for source in ["calc(1 / 2)", "random(0, 1)", "progress(1, 0, 2)"] {
            let ParseOutcome::NotHandled(reason) = parse(property_id::OPACITY, source) else {
                panic!("math function should fall back: {source}");
            };
            assert_eq!(reason.label, "calc");
        }
    }

    #[test]
    fn leaves_deferred_numeric_functions_with_cpp() {
        for (property, source, expected_reason) in [
            (property_id::OPACITY, "var(--opacity)", "substitution"),
            (
                property_id::FONT_SIZE,
                "attr(data-size type(<percentage>))",
                "substitution",
            ),
            (property_id::Z_INDEX, "sibling-index()", "function:not-ported"),
        ] {
            let ParseOutcome::NotHandled(reason) = parse(property, source) else {
                panic!("deferred function should fall back: {source}");
            };
            assert_eq!(reason.label, expected_reason);
        }
    }

    #[test]
    fn parses_css_wide_keywords_as_whole_values() {
        for (source, expected_keyword) in [
            ("initial", keyword::INITIAL),
            (" InHeRiT ", keyword::INHERIT),
            ("unset", keyword::UNSET),
            ("revert", keyword::REVERT),
            ("revert-layer", keyword::REVERT_LAYER),
        ] {
            let values = consume_a_list_of_component_values(&tokenize_for_parser(source.as_bytes())).unwrap();
            let ParseOutcome::Parsed(value) = parse_css_value(&context(), 1, &values) else {
                panic!("CSS-wide keyword should parse");
            };
            assert!(matches!(&*value, StyleValueData::Keyword { keyword } if *keyword == expected_keyword));
        }
    }

    #[test]
    fn does_not_parse_css_wide_keywords_as_partial_values() {
        let values = consume_a_list_of_component_values(&tokenize_for_parser(b"inherit extra")).unwrap();
        assert!(matches!(
            parse_css_value(&context(), 1, &values),
            ParseOutcome::NotHandled(_)
        ));
    }
}
