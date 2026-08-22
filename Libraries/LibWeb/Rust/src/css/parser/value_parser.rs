/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Parsed values use the thread-confined shared graph owned by the C++ style objects.
#![allow(clippy::arc_with_non_send_sync)]

use crate::css::css_enums::{
    keyword, keyword_from_ascii_case_insensitive, keyword_to_counter_style_name_keyword,
    keyword_to_cross_origin_modifier_value, keyword_to_cursor_predefined, keyword_to_referrer_policy_modifier_value,
    keyword_to_symbols_type, symbols_type,
};
use crate::css::css_pixels::CssPixels;
use crate::css::css_tokenizer::{CssNumberType, ParserTokenKind, tokenize_for_parser};
use crate::css::display::FfiDisplay;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::math_functions::math_function_from_name;
use crate::css::parser::calc_parser::{CalcParseError, parse_a_calc_function_node};
use crate::css::parser::color_parser::{is_color_function_name, parse_color_value};
use crate::css::parser::component_value::{ComponentKind, ComponentValue, consume_a_list_of_component_values};
use crate::css::parser::fonts_parser::{parse_font_descriptor, parse_font_property};
use crate::css::parser::grid_parser::parse_grid_property;
use crate::css::parser::images_gradients_parser::{is_image_function_name, parse_image_property, parse_image_value};
use crate::css::parser::positions_shapes_parser::{
    is_position_shape_function_name, parse_anchor_fit_property, parse_geometry_property, parse_position_property,
};
use crate::css::parser::token_stream::TokenStream;
use crate::css::parser::transforms_effects_parser::{
    is_transform_effect_function_name, parse_transform_effect_property,
};
use crate::css::property_metadata::{
    FIRST_SHORTHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, property_accepted_keywords, property_accepted_value_types,
    property_accepts_only_keywords, property_custom_ident_blacklist, property_has_coordinating_list_multiplicity,
    property_has_hashless_hex_color_quirk, property_has_unitless_length_quirk, property_id, property_is_shorthand,
    property_numeric_ranges, property_percentages_resolve_to, property_resolve_legacy_value_alias,
};
use crate::css::retained_fly_string::{RetainedUtf16FlyString, RetainedUtf16FlyStringList};
use crate::css::style_compute::{LENGTH_UNIT_NAMES, px_length_unit};
use crate::css::style_value::{
    RetainedByteList, RetainedCounterDefinition, RetainedCounterDefinitionList, RetainedNumericRangeList,
    RetainedRequestUrlModifier, RetainedRequestUrlModifierList, RetainedString, RetainedStyleValueData,
    RetainedStyleValueDataList, StyleValueData,
};
use std::collections::BTreeMap;
use std::ffi::c_void;
use std::sync::{Arc, Mutex, OnceLock};

include!(concat!(env!("OUT_DIR"), "/dimension_units_generated.rs"));

pub(crate) const PROPERTY_NOT_PORTED: NotHandledReason = NotHandledReason {
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
pub(crate) const SUBSTITUTION_NOT_PORTED: NotHandledReason = NotHandledReason {
    label: "substitution",
    c_label: b"substitution\0",
};
pub(crate) const FUNCTION_NOT_PORTED: NotHandledReason = NotHandledReason {
    label: "function:not-ported",
    c_label: b"function:not-ported\0",
};

// NB: Keep these in the order of the C++ ValueType enum.
pub(crate) const VALUE_TYPE_ANGLE: u8 = 2;
const VALUE_TYPE_COLOR: u8 = 6;
const VALUE_TYPE_CUSTOM_IDENT: u8 = 10;
const VALUE_TYPE_DASHED_IDENT: u8 = 11;
pub(crate) const VALUE_TYPE_FLEX: u8 = 15;
const VALUE_TYPE_FREQUENCY: u8 = 21;
const VALUE_TYPE_IMAGE: u8 = 23;
pub(crate) const VALUE_TYPE_INTEGER: u8 = 24;
pub(crate) const VALUE_TYPE_LENGTH: u8 = 25;
pub(crate) const VALUE_TYPE_NUMBER: u8 = 27;
const VALUE_TYPE_OPACITY_VALUE: u8 = 28;
pub(crate) const VALUE_TYPE_PERCENTAGE: u8 = 31;
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
#[derive(Clone, Copy, PartialEq, Eq)]
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
    pub normalize_svg_path_data: Option<unsafe extern "C" fn(*const u16, usize) -> usize>,
    pub font_format_is_supported: Option<unsafe extern "C" fn(*const u16, usize) -> bool>,
    pub font_tech_is_supported: Option<unsafe extern "C" fn(u8) -> bool>,
    pub random_function_index: *mut usize,
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
pub(crate) struct NumericRange {
    min: f64,
    max: f64,
}

impl NumericRange {
    pub(crate) const fn new(min: f64, max: f64) -> Self {
        Self { min, max }
    }

    pub(crate) const INFINITE: Self = Self {
        min: f32::MIN as f64,
        max: f32::MAX as f64,
    };
    pub(crate) const NON_NEGATIVE: Self = Self {
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

pub(crate) fn equals_ascii_case_insensitive(value: &[u16], expected: &[u8]) -> bool {
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

pub(crate) fn is_valid_custom_ident(identifier: &[u16], blacklist: &[&str]) -> bool {
    if keyword_from_ascii_case_insensitive(identifier).is_some_and(is_css_wide_keyword)
        || equals_ascii_case_insensitive(identifier, b"default")
    {
        return false;
    }
    !blacklist
        .iter()
        .any(|blocked| equals_ascii_case_insensitive(identifier, blocked.as_bytes()))
}

pub(crate) fn retain_fly_string(context: &ParseContext, string: &[u16]) -> Option<RetainedUtf16FlyString> {
    let callback = context.intern_utf16_fly_string?;
    let raw = unsafe { callback(string.as_ptr(), string.len()) };
    Some(unsafe { RetainedUtf16FlyString::from_leaked_raw(raw) })
}

pub(crate) fn string_style_value(context: &ParseContext, string: &[u16]) -> Option<StyleValueData> {
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

pub(crate) fn parse_url_value(context: &ParseContext, value: &ComponentValue) -> Option<StyleValueData> {
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

pub(crate) fn value_list(values: Vec<StyleValueData>, separator: u8, collapsible: bool) -> StyleValueData {
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

pub(crate) fn parse_integer_value(value: &ComponentValue, accepted_range: NumericRange) -> Option<StyleValueData> {
    let (value, number_type) = number_token(value)?;
    if number_type == CssNumberType::Number {
        return None;
    }
    let value = round_to_nearest_integer(value);
    accepted_range
        .contains(f64::from(value))
        .then_some(StyleValueData::Integer { value })
}

pub(crate) fn parse_number_value(value: &ComponentValue, accepted_range: NumericRange) -> Option<StyleValueData> {
    let (value, _) = number_token(value)?;
    accepted_range
        .contains(value)
        .then_some(StyleValueData::Number { value })
}

pub(crate) fn parse_percentage_value(value: &ComponentValue, accepted_range: NumericRange) -> Option<StyleValueData> {
    let ComponentKind::Token(ParserTokenKind::Percentage { value, .. }) = &value.kind else {
        return None;
    };
    let value = clamp_to_single_precision_range(*value);
    accepted_range
        .contains(value)
        .then_some(StyleValueData::Percentage { value })
}

pub(crate) fn parse_number_percentage_value(
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

pub(crate) fn parse_angle_value(
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

pub(crate) fn parse_angle_from_stream(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    accepted_range: NumericRange,
) -> Option<StyleValueData> {
    tokens.discard_whitespace();
    let value = tokens.next_token();
    let parsed = if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        parse_calculated_numeric_value_with_ranges(
            context,
            property,
            VALUE_TYPE_ANGLE,
            None,
            accepted_range,
            name,
            values,
        )
    } else {
        parse_angle_value(context, value, accepted_range)
    }?;
    tokens.discard_a_token();
    Some(parsed)
}

pub(crate) fn parse_angle_percentage_from_stream(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    accepted_angle_range: NumericRange,
    accepted_percentage_range: NumericRange,
) -> Option<StyleValueData> {
    tokens.discard_whitespace();
    let value = tokens.next_token();
    let parsed = if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        parse_calculated_numeric_value_with_ranges(
            context,
            property,
            VALUE_TYPE_ANGLE,
            Some(VALUE_TYPE_ANGLE),
            accepted_angle_range,
            name,
            values,
        )
        .or_else(|| {
            parse_calculated_numeric_value_with_ranges(
                context,
                property,
                VALUE_TYPE_PERCENTAGE,
                None,
                accepted_percentage_range,
                name,
                values,
            )
        })
    } else {
        parse_angle_percentage_value(context, value, accepted_angle_range, accepted_percentage_range)
    }?;
    tokens.discard_a_token();
    Some(parsed)
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

pub(crate) fn parse_flex_value(value: &ComponentValue, accepted_range: NumericRange) -> Option<StyleValueData> {
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

pub(crate) fn parse_length_value(
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

pub(crate) fn parse_length_percentage_value(
    context: &ParseContext,
    property: u16,
    value: &ComponentValue,
    accepted_length_range: NumericRange,
    accepted_percentage_range: NumericRange,
) -> Option<StyleValueData> {
    parse_length_value(context, property, value, accepted_length_range)
        .or_else(|| parse_percentage_value(value, accepted_percentage_range))
}

pub(crate) fn parse_length_from_stream(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    accepted_range: NumericRange,
) -> Option<StyleValueData> {
    tokens.discard_whitespace();
    let value = tokens.next_token();
    let parsed = if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        parse_calculated_numeric_value_with_ranges(
            context,
            property,
            VALUE_TYPE_LENGTH,
            None,
            accepted_range,
            name,
            values,
        )
    } else {
        parse_length_value(context, property, value, accepted_range)
    }?;
    tokens.discard_a_token();
    Some(parsed)
}

pub(crate) fn parse_length_percentage_from_stream(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    accepted_length_range: NumericRange,
    accepted_percentage_range: NumericRange,
) -> Option<StyleValueData> {
    tokens.discard_whitespace();
    let value = tokens.next_token();
    let parsed = if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        parse_calculated_numeric_value_with_ranges(
            context,
            property,
            VALUE_TYPE_LENGTH,
            Some(VALUE_TYPE_LENGTH),
            accepted_length_range,
            name,
            values,
        )
        .or_else(|| {
            parse_calculated_numeric_value_with_ranges(
                context,
                property,
                VALUE_TYPE_PERCENTAGE,
                None,
                accepted_percentage_range,
                name,
                values,
            )
        })
    } else {
        parse_length_percentage_value(
            context,
            property,
            value,
            accepted_length_range,
            accepted_percentage_range,
        )
    }?;
    tokens.discard_a_token();
    Some(parsed)
}

pub(crate) fn parse_number_from_stream(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    accepted_range: NumericRange,
) -> Option<StyleValueData> {
    tokens.discard_whitespace();
    let value = tokens.next_token();
    let parsed = if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        parse_calculated_numeric_value_with_ranges(
            context,
            property,
            VALUE_TYPE_NUMBER,
            None,
            accepted_range,
            name,
            values,
        )
    } else {
        parse_number_value(value, accepted_range)
    }?;
    tokens.discard_a_token();
    Some(parsed)
}

pub(crate) fn parse_integer_from_stream(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    accepted_range: NumericRange,
) -> Option<StyleValueData> {
    tokens.discard_whitespace();
    let value = tokens.next_token();
    let parsed = if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        parse_calculated_numeric_value_with_ranges(
            context,
            property,
            VALUE_TYPE_INTEGER,
            None,
            accepted_range,
            name,
            values,
        )
    } else {
        parse_integer_value(value, accepted_range)
    }?;
    tokens.discard_a_token();
    Some(parsed)
}

pub(crate) fn parse_resolution_value(value: &ComponentValue, accepted_range: NumericRange) -> Option<StyleValueData> {
    parse_dimension_value(value, VALUE_TYPE_RESOLUTION, accepted_range)
}

pub(crate) fn parse_resolution_from_stream(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    accepted_range: NumericRange,
) -> Option<StyleValueData> {
    tokens.discard_whitespace();
    let value = tokens.next_token();
    let parsed = if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        parse_calculated_numeric_value_with_ranges(
            context,
            property,
            VALUE_TYPE_RESOLUTION,
            None,
            accepted_range,
            name,
            values,
        )
    } else {
        parse_resolution_value(value, accepted_range)
    }?;
    tokens.discard_a_token();
    Some(parsed)
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

pub(crate) fn is_arbitrary_substitution_function(name: &[u16]) -> bool {
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
            } else if math_function_from_name(name).is_some()
                || is_color_function_name(name)
                || is_position_shape_function_name(name)
                || is_transform_effect_function_name(name)
                || is_image_function_name(name)
            {
                unported_function_reason(values)
            } else {
                // NB: C++ numeric grammars can accept functions such as
                //     anchor-size(). Until Rust parses a function itself, it
                //     cannot authoritatively reject the enclosing value.
                Some(&FUNCTION_NOT_PORTED)
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
    if let Some((name, values)) = value.function()
        && values.iter().all(ComponentValue::is_whitespace)
        && context_allows_tree_counting_functions(context)
    {
        let function = if equals_ascii_case_insensitive(name, b"sibling-count") {
            0
        } else if equals_ascii_case_insensitive(name, b"sibling-index") {
            1
        } else {
            u8::MAX
        };
        if function != u8::MAX && matches!(value_type, VALUE_TYPE_NUMBER | VALUE_TYPE_INTEGER) {
            return Some(StyleValueData::TreeCountingFunction {
                function,
                computed_type: u8::from(value_type == VALUE_TYPE_INTEGER),
            });
        }
    }
    if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        return parse_calculated_numeric_value(context, property, value_type, name, values);
    }
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

pub(crate) fn context_allows_tree_counting_functions(context: &ParseContext) -> bool {
    let contexts = if context.value_context_count == 0 {
        &[]
    } else if context.value_contexts.is_null() {
        return false;
    } else {
        unsafe { std::slice::from_raw_parts(context.value_contexts, context.value_context_count) }
    };
    !contexts.iter().any(|value_context| {
        value_context.kind == FfiValueParsingContextKind::Descriptor
            || (value_context.kind == FfiValueParsingContextKind::Special && matches!(value_context.value, 0..=2))
    })
}

fn parse_calculated_numeric_value(
    context: &ParseContext,
    property: u16,
    value_type: u8,
    name: &[u16],
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let property_percentages_resolve_as = property_percentages_resolve_to(property);
    let percentages_resolve_as = (property_percentages_resolve_as == Some(value_type)).then_some(value_type);
    let range = if value_type == VALUE_TYPE_OPACITY_VALUE {
        NumericRange::INFINITE
    } else {
        accepted_range(property, value_type)
    };
    parse_calculated_numeric_value_with_ranges(
        context,
        property,
        value_type,
        percentages_resolve_as,
        range,
        name,
        values,
    )
}

pub(crate) fn parse_calculated_numeric_value_with_ranges(
    context: &ParseContext,
    property: u16,
    value_type: u8,
    percentages_resolve_as: Option<u8>,
    range: NumericRange,
    name: &[u16],
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    if equals_ascii_case_insensitive(name, b"calc")
        && let Some(tree_value) = single_non_whitespace_value(values)
        && let Some((tree_name, tree_arguments)) = tree_value.function()
        && tree_arguments.iter().all(ComponentValue::is_whitespace)
        && context_allows_tree_counting_functions(context)
    {
        let function = if equals_ascii_case_insensitive(tree_name, b"sibling-count") {
            0
        } else if equals_ascii_case_insensitive(tree_name, b"sibling-index") {
            1
        } else {
            u8::MAX
        };
        if function != u8::MAX && matches!(value_type, VALUE_TYPE_NUMBER | VALUE_TYPE_INTEGER) {
            return Some(StyleValueData::TreeCountingFunction {
                function,
                computed_type: u8::from(value_type == VALUE_TYPE_INTEGER),
            });
        }
    }
    let root = match parse_a_calc_function_node(
        name,
        values,
        crate::css::parser::calc_parser::CalcParserContext {
            percentages_resolve_as,
            property,
            random_function_index: context.random_function_index,
            intern_utf16_fly_string: context.intern_utf16_fly_string,
            allowed_color_channels: 0,
            allow_random_functions: context_allows_random_functions(context),
            parse_context: context,
        },
    ) {
        Ok(root) => root,
        Err(CalcParseError::Invalid | CalcParseError::NotHandled) => return None,
    };
    let (_, numeric_type) = crate::css::calc::simplify_parsed_calculation(root.clone(), percentages_resolve_as)?;
    let resolve_as = crate::css::calc::resolve_as_for_value_type(percentages_resolve_as);
    let opacity_resolved_type = if value_type == VALUE_TYPE_OPACITY_VALUE {
        if numeric_type.matches_number(resolve_as) {
            Some(VALUE_TYPE_NUMBER)
        } else if numeric_type.matches_percentage() {
            Some(VALUE_TYPE_PERCENTAGE)
        } else {
            None
        }
    } else {
        None
    };
    let matches = match value_type {
        VALUE_TYPE_OPACITY_VALUE => opacity_resolved_type.is_some(),
        VALUE_TYPE_INTEGER | VALUE_TYPE_NUMBER => numeric_type.matches_number(resolve_as),
        VALUE_TYPE_ANGLE => {
            numeric_type.matches_dimension(1, resolve_as)
                || (percentages_resolve_as == Some(VALUE_TYPE_ANGLE) && numeric_type.matches_percentage())
        }
        VALUE_TYPE_FLEX => numeric_type.matches_dimension(5, resolve_as),
        VALUE_TYPE_FREQUENCY => {
            numeric_type.matches_dimension(3, resolve_as)
                || (percentages_resolve_as == Some(VALUE_TYPE_FREQUENCY) && numeric_type.matches_percentage())
        }
        VALUE_TYPE_LENGTH => {
            numeric_type.matches_dimension(0, resolve_as)
                || (percentages_resolve_as == Some(VALUE_TYPE_LENGTH) && numeric_type.matches_percentage())
        }
        VALUE_TYPE_RESOLUTION => numeric_type.matches_dimension(4, resolve_as),
        VALUE_TYPE_TIME => {
            numeric_type.matches_dimension(2, resolve_as)
                || (percentages_resolve_as == Some(VALUE_TYPE_TIME) && numeric_type.matches_percentage())
        }
        VALUE_TYPE_PERCENTAGE => numeric_type.matches_percentage(),
        _ => false,
    };
    if !matches {
        return None;
    }
    let (range_value_type, range) = opacity_resolved_type
        .map(|value_type| (value_type, NumericRange::INFINITE))
        .unwrap_or((value_type, range));
    let calculated = StyleValueData::Calculated {
        rust_calculation: crate::css::calc::CalcNodeHandle::from_arc(root),
        resolve_as_is_number: percentages_resolve_as == Some(VALUE_TYPE_NUMBER),
        resolve_as_base: resolve_as
            .and_then(|resolve_as| match resolve_as {
                crate::css::calc::ResolveAs::Base(base) => Some(base),
                crate::css::calc::ResolveAs::Number => None,
            })
            .unwrap_or(0),
        resolved_type: crate::css::calc::FfiNumericType::from_calc(Some(numeric_type)),
        has_percentages_resolve_as: percentages_resolve_as.is_some(),
        percentages_resolve_as: percentages_resolve_as.unwrap_or(0),
        resolve_numbers_as_integers: value_type == VALUE_TYPE_INTEGER,
        accepted_ranges: RetainedNumericRangeList::from_single_numeric_range(range_value_type, range.min, range.max),
    };
    if value_type == VALUE_TYPE_OPACITY_VALUE {
        Some(StyleValueData::OpacityValue {
            value: RetainedStyleValueData::from_owned(calculated),
        })
    } else {
        Some(calculated)
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

fn parse_color_scheme_property(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let non_whitespace = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    if non_whitespace.len() == 1
        && non_whitespace[0]
            .ident()
            .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"normal"))
    {
        return ParseOutcome::Parsed(Arc::new(StyleValueData::ColorScheme {
            schemes: RetainedUtf16FlyStringList::from_retained_strings(Vec::new()),
            scheme_codes: RetainedByteList::from_bytes(Vec::new()),
            only: false,
        }));
    }

    let mut only = false;
    let mut schemes = Vec::new();
    let mut scheme_codes = Vec::new();
    for (index, value) in non_whitespace.iter().enumerate() {
        let Some(identifier) = value.ident() else {
            return ParseOutcome::Invalid;
        };
        if equals_ascii_case_insensitive(identifier, b"only") {
            if only || (index != 0 && index + 1 != non_whitespace.len()) {
                return ParseOutcome::Invalid;
            }
            only = true;
            continue;
        }
        if !is_valid_custom_ident(identifier, &["normal"]) {
            return ParseOutcome::Invalid;
        }
        schemes.push(retain_fly_string(context, identifier).expect("parse context supplies string interning"));
        scheme_codes.push(if equals_ascii_case_insensitive(identifier, b"dark") {
            1
        } else if equals_ascii_case_insensitive(identifier, b"light") {
            2
        } else {
            0
        });
    }
    if schemes.is_empty() {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::Parsed(Arc::new(StyleValueData::ColorScheme {
        schemes: RetainedUtf16FlyStringList::from_retained_strings(schemes),
        scheme_codes: RetainedByteList::from_bytes(scheme_codes),
        only,
    }))
}

fn parse_quotes_property(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    if let Some(value) = single_non_whitespace_value(values)
        && let Some(keyword) = parse_specific_keyword(value, &[keyword::AUTO, keyword::NONE])
    {
        return ParseOutcome::Parsed(Arc::new(keyword));
    }
    let strings = values
        .iter()
        .filter(|value| !value.is_whitespace())
        .map(|value| string_style_value(context, value.string()?))
        .collect::<Option<Vec<_>>>();
    let Some(strings) = strings else {
        return ParseOutcome::Invalid;
    };
    if strings.len() % 2 != 0 {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::Parsed(Arc::new(value_list(strings, 0, false)))
}

fn parse_counter_definitions_property(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    if let Some(value) = single_non_whitespace_value(values)
        && let Some(none) = parse_specific_keyword(value, &[keyword::NONE])
    {
        return ParseOutcome::Parsed(Arc::new(none));
    }

    let allow_reversed = property == property_id::COUNTER_RESET;
    let default_value = i32::from(property == property_id::COUNTER_INCREMENT);
    let mut tokens = TokenStream::new(values);
    let mut definitions = Vec::new();
    tokens.discard_whitespace();
    while tokens.has_next_token() {
        let (name, is_reversed) = if let Some(identifier) = tokens.next_token().ident() {
            if !is_valid_custom_ident(identifier, &["none"]) {
                return ParseOutcome::Invalid;
            }
            let name = retain_fly_string(context, identifier).expect("parse context supplies string interning");
            tokens.discard_a_token();
            (name, false)
        } else if allow_reversed {
            let Some((name, arguments)) = tokens.next_token().function() else {
                return ParseOutcome::Invalid;
            };
            if !equals_ascii_case_insensitive(name, b"reversed") {
                return ParseOutcome::Invalid;
            }
            let Some(identifier) = single_non_whitespace_value(arguments).and_then(ComponentValue::ident) else {
                return ParseOutcome::Invalid;
            };
            if !is_valid_custom_ident(identifier, &["none"]) {
                return ParseOutcome::Invalid;
            }
            let name = retain_fly_string(context, identifier).expect("parse context supplies string interning");
            tokens.discard_a_token();
            (name, true)
        } else {
            return ParseOutcome::Invalid;
        };
        tokens.discard_whitespace();
        let value = if tokens.has_next_token() {
            let mut candidate = tokens.clone();
            parse_integer_from_stream(context, property, &mut candidate, NumericRange::INFINITE).map(|value| {
                tokens = candidate;
                RetainedStyleValueData::from_owned(value)
            })
        } else {
            None
        };
        let value = value.unwrap_or_else(|| {
            if is_reversed {
                RetainedStyleValueData::none()
            } else {
                RetainedStyleValueData::from_owned(StyleValueData::Integer { value: default_value })
            }
        });
        definitions.push(RetainedCounterDefinition::new(name, is_reversed, value));
        tokens.discard_whitespace();
    }
    if definitions.is_empty() {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::Parsed(Arc::new(StyleValueData::CounterDefinitions {
        counter_definitions: RetainedCounterDefinitionList::from_retained_elements(definitions),
    }))
}

fn parse_counter_style(context: &ParseContext, value: &ComponentValue) -> Option<StyleValueData> {
    if let Some(identifier) = value.ident() {
        if !is_valid_custom_ident(identifier, &["none"]) {
            return None;
        }
        let name = if keyword_from_ascii_case_insensitive(identifier)
            .and_then(keyword_to_counter_style_name_keyword)
            .is_some()
        {
            identifier
                .iter()
                .map(|&code_unit| {
                    if (u16::from(b'A')..=u16::from(b'Z')).contains(&code_unit) {
                        code_unit + u16::from(b'a' - b'A')
                    } else {
                        code_unit
                    }
                })
                .collect::<Vec<_>>()
        } else {
            identifier.to_vec()
        };
        return Some(StyleValueData::CounterStyle {
            is_symbols: false,
            name: retain_fly_string(context, &name)?,
            symbols_type: symbols_type::SYMBOLIC,
            symbols: RetainedUtf16FlyStringList::from_retained_strings(Vec::new()),
        });
    }

    let (name, arguments) = value.function()?;
    if !equals_ascii_case_insensitive(name, b"symbols") {
        return None;
    }
    let mut tokens = TokenStream::new(arguments);
    tokens.discard_whitespace();
    let mut symbol_type = symbols_type::SYMBOLIC;
    if let Some(keyword) = tokens
        .next_token()
        .ident()
        .and_then(keyword_from_ascii_case_insensitive)
        .and_then(keyword_to_symbols_type)
    {
        symbol_type = keyword;
        tokens.discard_a_token();
    }
    tokens.discard_whitespace();
    let mut symbols = Vec::new();
    while let Some(string) = tokens.next_token().string() {
        symbols.push(retain_fly_string(context, string)?);
        tokens.discard_a_token();
        tokens.discard_whitespace();
    }
    if tokens.has_next_token()
        || symbols.is_empty()
        || (matches!(symbol_type, symbols_type::ALPHABETIC | symbols_type::NUMERIC) && symbols.len() < 2)
    {
        return None;
    }
    Some(StyleValueData::CounterStyle {
        is_symbols: true,
        name: RetainedUtf16FlyString::none(),
        symbols_type: symbol_type,
        symbols: RetainedUtf16FlyStringList::from_retained_strings(symbols),
    })
}

fn parse_list_style_type_property(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let parsed = single_non_whitespace_value(values).and_then(|value| {
        parse_specific_keyword(value, &[keyword::NONE])
            .or_else(|| string_style_value(context, value.string()?))
            .or_else(|| parse_counter_style(context, value))
    });
    parsed.map_or(ParseOutcome::Invalid, |parsed| ParseOutcome::Parsed(Arc::new(parsed)))
}

fn parse_counter_function(context: &ParseContext, value: &ComponentValue) -> Option<StyleValueData> {
    let (name, arguments) = value.function()?;
    let function = if equals_ascii_case_insensitive(name, b"counter") {
        0
    } else if equals_ascii_case_insensitive(name, b"counters") {
        1
    } else {
        return None;
    };
    let parts = arguments.split(ComponentValue::is_comma).collect::<Vec<_>>();
    if parts.len() < function + 1 || parts.len() > function + 2 {
        return None;
    }
    let counter_name = single_non_whitespace_value(parts[0])?.ident()?;
    if !is_valid_custom_ident(counter_name, &["none"]) {
        return None;
    }
    let join_string = if function == 1 {
        retain_fly_string(context, single_non_whitespace_value(parts[1])?.string()?)?
    } else {
        retain_fly_string(context, &[])?
    };
    let style_index = function + 1;
    let counter_style = if parts.len() > style_index {
        parse_counter_style(context, single_non_whitespace_value(parts[style_index])?)?
    } else {
        StyleValueData::CounterStyle {
            is_symbols: false,
            name: retain_fly_string(context, &"decimal".encode_utf16().collect::<Vec<_>>())?,
            symbols_type: symbols_type::SYMBOLIC,
            symbols: RetainedUtf16FlyStringList::from_retained_strings(Vec::new()),
        }
    };
    Some(StyleValueData::Counter {
        function: function as u8,
        counter_name: retain_fly_string(context, counter_name)?,
        counter_style: RetainedStyleValueData::from_owned(counter_style),
        join_string,
    })
}

fn parse_content_property(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let non_whitespace_values = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    if non_whitespace_values.len() > 1
        && non_whitespace_values
            .iter()
            .all(|value| value.string().is_some_and(<[u16]>::is_empty))
    {
        // NB: Parser-synthesized StyleValueList tokens are reconstructed as separate empty strings for Rust,
        //     while the C++ token stream retains the original single list value. Task 016 removes this fragment seam.
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    }
    if let Some(value) = single_non_whitespace_value(values)
        && let Some(keyword) = parse_specific_keyword(value, &[keyword::NONE, keyword::NORMAL])
    {
        return ParseOutcome::Parsed(Arc::new(keyword));
    }
    let mut tokens = TokenStream::new(values);
    let mut content = Vec::new();
    let mut alt_text = Vec::new();
    let mut in_alt_text = false;
    tokens.discard_whitespace();
    while tokens.has_next_token() {
        if tokens.next_token().is_delim(b'/') {
            if in_alt_text || content.is_empty() {
                return ParseOutcome::Invalid;
            }
            in_alt_text = true;
            tokens.discard_a_token();
            tokens.discard_whitespace();
            continue;
        }
        let value = tokens.next_token();
        let parsed = string_style_value(context, value.string().unwrap_or(&[]))
            .filter(|_| value.string().is_some())
            .or_else(|| parse_counter_function(context, value))
            .or_else(|| {
                let identifier = value.ident()?;
                let keyword = keyword_from_ascii_case_insensitive(identifier)?;
                property_accepted_keywords(property_id::CONTENT)
                    .binary_search(&keyword)
                    .is_ok()
                    .then_some(StyleValueData::Keyword { keyword })
            });
        let parsed = if let Some(parsed) = parsed {
            tokens.discard_a_token();
            parsed
        } else if !in_alt_text {
            let mut candidate = tokens.clone();
            let Some(image) = parse_image_value(context, property_id::CONTENT, &mut candidate, true) else {
                return ParseOutcome::Invalid;
            };
            tokens = candidate;
            image
        } else {
            return ParseOutcome::Invalid;
        };
        if matches!(
            parsed,
            StyleValueData::Keyword {
                keyword: keyword::NONE | keyword::NORMAL
            }
        ) {
            return ParseOutcome::Invalid;
        }
        if in_alt_text && !matches!(parsed, StyleValueData::String { .. } | StyleValueData::Counter { .. }) {
            return ParseOutcome::Invalid;
        }
        if in_alt_text {
            alt_text.push(parsed);
        } else {
            content.push(parsed);
        }
        tokens.discard_whitespace();
    }
    if content.is_empty() || (in_alt_text && alt_text.is_empty()) {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::Parsed(Arc::new(StyleValueData::Content {
        content: RetainedStyleValueData::from_owned(value_list(content, 0, false)),
        alt_text: if alt_text.is_empty() {
            RetainedStyleValueData::none()
        } else {
            RetainedStyleValueData::from_owned(value_list(alt_text, 0, false))
        },
    }))
}

fn parse_cursor_property(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let parts = values.split(ComponentValue::is_comma).collect::<Vec<_>>();
    if parts.is_empty() {
        return ParseOutcome::Invalid;
    }
    let mut cursors = Vec::new();
    for (index, part) in parts.iter().enumerate() {
        let mut tokens = TokenStream::new(part);
        tokens.discard_whitespace();
        if index + 1 == parts.len() {
            let Some(keyword) = tokens
                .next_token()
                .ident()
                .and_then(keyword_from_ascii_case_insensitive)
                .filter(|keyword| keyword_to_cursor_predefined(*keyword).is_some())
            else {
                return ParseOutcome::Invalid;
            };
            tokens.discard_a_token();
            tokens.discard_whitespace();
            if tokens.has_next_token() {
                return ParseOutcome::Invalid;
            }
            cursors.push(StyleValueData::Keyword { keyword });
            continue;
        }
        let Some(image) = parse_image_value(context, property_id::CURSOR, &mut tokens, true) else {
            return ParseOutcome::Invalid;
        };
        tokens.discard_whitespace();
        let (x, y) = if tokens.has_next_token() {
            let Some(x) = parse_number_from_stream(context, property_id::CURSOR, &mut tokens, NumericRange::INFINITE)
            else {
                return ParseOutcome::Invalid;
            };
            let Some(y) = parse_number_from_stream(context, property_id::CURSOR, &mut tokens, NumericRange::INFINITE)
            else {
                return ParseOutcome::Invalid;
            };
            (
                RetainedStyleValueData::from_owned(x),
                RetainedStyleValueData::from_owned(y),
            )
        } else {
            (RetainedStyleValueData::none(), RetainedStyleValueData::none())
        };
        tokens.discard_whitespace();
        if tokens.has_next_token() {
            return ParseOutcome::Invalid;
        }
        cursors.push(StyleValueData::Cursor {
            image: RetainedStyleValueData::from_owned(image),
            x,
            y,
        });
    }
    ParseOutcome::Parsed(Arc::new(match cursors.as_slice() {
        [_] => cursors.remove(0),
        _ => value_list(cursors, 1, true),
    }))
}

fn parse_view_timeline_inset_from_stream(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<StyleValueData> {
    let mut inset_values = Vec::new();
    while inset_values.len() < 2 {
        tokens.discard_whitespace();
        if let Some(auto) = parse_specific_keyword(tokens.next_token(), &[keyword::AUTO]) {
            tokens.discard_a_token();
            inset_values.push(auto);
            continue;
        }
        let mut candidate = tokens.clone();
        let Some(inset) = parse_length_percentage_from_stream(
            context,
            property,
            &mut candidate,
            NumericRange::INFINITE,
            NumericRange::INFINITE,
        ) else {
            break;
        };
        *tokens = candidate;
        inset_values.push(inset);
    }
    if inset_values.len() == 1 {
        inset_values.push(inset_values[0].clone());
    }
    (!inset_values.is_empty()).then(|| value_list(inset_values, 0, true))
}

fn retained_function(
    context: &ParseContext,
    name: &[u8],
    arguments: Vec<RetainedStyleValueData>,
) -> Option<StyleValueData> {
    Some(StyleValueData::Function {
        name: retain_fly_string(context, &name.iter().map(|byte| u16::from(*byte)).collect::<Vec<_>>())?,
        value: RetainedStyleValueData::from_owned(StyleValueData::Tuple {
            values: RetainedStyleValueDataList::from_retained_values(arguments),
        }),
    })
}

fn parse_scroll_function(context: &ParseContext, value: &ComponentValue) -> Option<StyleValueData> {
    let (name, arguments) = value.function()?;
    if !equals_ascii_case_insensitive(name, b"scroll") {
        return None;
    }
    let mut scroller = None;
    let mut axis = None;
    let mut has_scroller = false;
    let mut has_axis = false;
    for value in arguments.iter().filter(|value| !value.is_whitespace()) {
        let parsed = parse_property_keyword_data(property_id::ANIMATION_TIMELINE, value).or_else(|| {
            value
                .ident()
                .and_then(keyword_from_ascii_case_insensitive)
                .map(|keyword| StyleValueData::Keyword { keyword })
        })?;
        let StyleValueData::Keyword { keyword } = parsed else {
            return None;
        };
        if matches!(keyword, keyword::ROOT | keyword::NEAREST | keyword::SELF) {
            if has_scroller {
                return None;
            }
            has_scroller = true;
            scroller = (keyword != keyword::NEAREST).then_some(StyleValueData::Keyword { keyword });
        } else if matches!(keyword, keyword::BLOCK | keyword::INLINE | keyword::X | keyword::Y) {
            if has_axis {
                return None;
            }
            has_axis = true;
            axis = (keyword != keyword::BLOCK).then_some(StyleValueData::Keyword { keyword });
        } else {
            return None;
        }
    }
    retained_function(
        context,
        b"scroll",
        vec![
            scroller.map_or_else(RetainedStyleValueData::none, RetainedStyleValueData::from_owned),
            axis.map_or_else(RetainedStyleValueData::none, RetainedStyleValueData::from_owned),
        ],
    )
}

fn parse_view_function(context: &ParseContext, property: u16, value: &ComponentValue) -> Option<StyleValueData> {
    let (name, arguments) = value.function()?;
    if !equals_ascii_case_insensitive(name, b"view") {
        return None;
    }
    let mut tokens = TokenStream::new(arguments);
    let mut axis = None;
    let mut inset = None;
    let mut has_axis = false;
    let mut has_inset = false;
    while {
        tokens.discard_whitespace();
        tokens.has_next_token()
    } {
        let mut candidate = tokens.clone();
        if let Some(parsed_inset) = parse_view_timeline_inset_from_stream(context, property, &mut candidate) {
            if has_inset {
                return None;
            }
            has_inset = true;
            tokens = candidate;
            let is_default = matches!(
                &parsed_inset,
                StyleValueData::ValueList { values, .. }
                    if values.as_slice().iter().all(|value| matches!(value.data(), StyleValueData::Keyword { keyword: keyword::AUTO }))
            );
            inset = (!is_default).then_some(parsed_inset);
            continue;
        }
        let keyword = tokens
            .next_token()
            .ident()
            .and_then(keyword_from_ascii_case_insensitive)?;
        if !matches!(keyword, keyword::BLOCK | keyword::INLINE | keyword::X | keyword::Y) || has_axis {
            return None;
        }
        has_axis = true;
        tokens.discard_a_token();
        axis = (keyword != keyword::BLOCK).then_some(StyleValueData::Keyword { keyword });
    }
    retained_function(
        context,
        b"view",
        vec![
            axis.map_or_else(RetainedStyleValueData::none, RetainedStyleValueData::from_owned),
            inset.map_or_else(RetainedStyleValueData::none, RetainedStyleValueData::from_owned),
        ],
    )
}

fn parse_animation_timeline_property(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let timelines = values
        .split(ComponentValue::is_comma)
        .map(|item| {
            let value = single_non_whitespace_value(item)?;
            parse_specific_keyword(value, &[keyword::AUTO, keyword::NONE])
                .or_else(|| parse_dashed_ident_value(context, value))
                .or_else(|| parse_scroll_function(context, value))
                .or_else(|| parse_view_function(context, property_id::ANIMATION_TIMELINE, value))
        })
        .collect::<Option<Vec<_>>>();
    timelines.map_or(ParseOutcome::Invalid, |timelines| {
        ParseOutcome::Parsed(Arc::new(value_list(timelines, 1, true)))
    })
}

fn parse_view_timeline_inset_property(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    let insets = values
        .split(ComponentValue::is_comma)
        .map(|item| {
            let mut tokens = TokenStream::new(item);
            let parsed = parse_view_timeline_inset_from_stream(context, property, &mut tokens)?;
            tokens.discard_whitespace();
            (!tokens.has_next_token()).then_some(parsed)
        })
        .collect::<Option<Vec<_>>>();
    insets.map_or(ParseOutcome::Invalid, |insets| {
        ParseOutcome::Parsed(Arc::new(value_list(insets, 1, true)))
    })
}

fn parse_long_tail_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    match property {
        property_id::ANIMATION_TIMELINE => parse_animation_timeline_property(context, values),
        property_id::COLOR_SCHEME => parse_color_scheme_property(context, values),
        property_id::CONTENT => parse_content_property(context, values),
        property_id::CURSOR => parse_cursor_property(context, values),
        property_id::LIST_STYLE_TYPE => parse_list_style_type_property(context, values),
        property_id::QUOTES => parse_quotes_property(context, values),
        property_id::VIEW_TIMELINE_INSET => parse_view_timeline_inset_property(context, property, values),
        property_id::COUNTER_INCREMENT | property_id::COUNTER_RESET | property_id::COUNTER_SET => {
            if values.iter().any(|value| {
                value
                    .function()
                    .is_some_and(|(name, _)| math_function_from_name(name).is_some())
            }) && unported_function_reason(values).is_some()
                && !contains_tree_counting_function(values)
            {
                return ParseOutcome::NotHandled(&FUNCTION_NOT_PORTED);
            }
            parse_counter_definitions_property(context, property, values)
        }
        _ => ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED),
    }
}

fn contains_tree_counting_function(values: &[ComponentValue]) -> bool {
    values.iter().any(|value| match &value.kind {
        ComponentKind::Function { name, values } => {
            equals_ascii_case_insensitive(name, b"sibling-count")
                || equals_ascii_case_insensitive(name, b"sibling-index")
                || contains_tree_counting_function(values)
        }
        ComponentKind::SimpleBlock { values, .. } => contains_tree_counting_function(values),
        ComponentKind::Token(_) => false,
    })
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
    if let Some(reason) = unported_function_reason(values)
        && !contains_tree_counting_function(values)
    {
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

fn context_allows_quirky_color(context: &ParseContext, property: u16) -> bool {
    if !context.in_quirks_mode || !property_has_hashless_hex_color_quirk(property) {
        return false;
    }
    let value_contexts = if context.value_context_count == 0 {
        &[]
    } else if context.value_contexts.is_null() {
        return false;
    } else {
        unsafe { std::slice::from_raw_parts(context.value_contexts, context.value_context_count) }
    };
    value_contexts.iter().all(|value_context| {
        value_context.kind == FfiValueParsingContextKind::Property
            && property_has_hashless_hex_color_quirk(value_context.value)
    })
}

pub(crate) fn context_allows_random_functions(context: &ParseContext) -> bool {
    let value_contexts = if context.value_context_count == 0 {
        &[]
    } else if context.value_contexts.is_null() {
        return false;
    } else {
        unsafe { std::slice::from_raw_parts(context.value_contexts, context.value_context_count) }
    };

    const CANVAS_CONTEXT_GENERIC_VALUE: u16 = 0;
    const ON_SCREEN_CANVAS_CONTEXT_FONT_VALUE: u16 = 3;
    if value_contexts.first().is_some_and(|value_context| {
        value_context.kind == FfiValueParsingContextKind::Special
            && matches!(
                value_context.value,
                CANVAS_CONTEXT_GENERIC_VALUE | ON_SCREEN_CANVAS_CONTEXT_FONT_VALUE
            )
    }) {
        return false;
    }

    value_contexts
        .iter()
        .any(|value_context| value_context.kind == FfiValueParsingContextKind::Property)
}

fn parse_color_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if !(FIRST_SHORTHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property)
        || property_is_shorthand(property)
        || !property_accepted_value_types(property).contains(&VALUE_TYPE_COLOR)
    {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    }
    let mut stream = TokenStream::new(values);
    let Some(color) = parse_color_value(
        context,
        property,
        &mut stream,
        context_allows_quirky_color(context, property),
    ) else {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    };
    stream.discard_whitespace();
    if stream.has_next_token() {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    }
    ParseOutcome::Parsed(Arc::new(color))
}

/// Parse a property value using the grammars which have been ported to Rust.
///
/// `Invalid` is reserved for grammars which Rust handles completely. Until a
/// grammar is ported, C++ remains authoritative through `NotHandled`.
pub(crate) fn parse_css_value(context: &ParseContext, property_id: u16, values: &[ComponentValue]) -> ParseOutcome {
    if let Some(value) = parse_builtin_value(values) {
        return ParseOutcome::Parsed(Arc::new(value));
    }
    let font_outcome = parse_font_property(context, property_id, values);
    if !matches!(font_outcome, ParseOutcome::NotHandled(_)) {
        return font_outcome;
    }
    let grid_outcome = parse_grid_property(context, property_id, values);
    if !matches!(grid_outcome, ParseOutcome::NotHandled(_)) {
        return grid_outcome;
    }
    if matches!(unported_function_reason(values), Some(reason) if reason.label == SUBSTITUTION_NOT_PORTED.label) {
        return ParseOutcome::NotHandled(&SUBSTITUTION_NOT_PORTED);
    }
    let long_tail_outcome = parse_long_tail_property(context, property_id, values);
    if !matches!(long_tail_outcome, ParseOutcome::NotHandled(_)) {
        return long_tail_outcome;
    }
    if contains_tree_counting_function(values) {
        let tree_counting_outcome = parse_generic_numeric_property(context, property_id, values);
        if !matches!(tree_counting_outcome, ParseOutcome::NotHandled(_)) {
            return tree_counting_outcome;
        }
    }
    if let Some(reason) = unported_function_reason(values) {
        return ParseOutcome::NotHandled(reason);
    }
    if !(FIRST_SHORTHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property_id) {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    }
    let transform_effect_outcome = parse_transform_effect_property(context, property_id, values);
    if !matches!(transform_effect_outcome, ParseOutcome::NotHandled(_)) {
        return transform_effect_outcome;
    }
    let image_outcome = parse_image_property(context, property_id, values);
    if !matches!(image_outcome, ParseOutcome::NotHandled(_)) {
        return image_outcome;
    }
    let position_outcome = parse_position_property(context, property_id, values);
    if !matches!(position_outcome, ParseOutcome::NotHandled(_)) {
        return position_outcome;
    }
    let geometry_outcome = parse_geometry_property(context, property_id, values);
    if !matches!(geometry_outcome, ParseOutcome::NotHandled(_)) {
        return geometry_outcome;
    }
    let anchor_fit_outcome = parse_anchor_fit_property(context, property_id, values);
    if !matches!(anchor_fit_outcome, ParseOutcome::NotHandled(_)) {
        return anchor_fit_outcome;
    }
    if property_id == property_id::DISPLAY {
        return parse_display_keyword(values);
    }
    let color_outcome = parse_color_property(context, property_id, values);
    if !matches!(color_outcome, ParseOutcome::NotHandled(_)) {
        return color_outcome;
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

#[repr(u8)]
#[derive(Clone, Copy)]
#[allow(dead_code)]
pub enum FfiFontDescriptorKind {
    FamilyName,
    SourceList,
    UnicodeRangeList,
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

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_font_descriptor(
    context: *const ParseContext,
    kind: FfiFontDescriptorKind,
    source: FfiUtf16View,
    out_status: *mut FfiParseStatus,
) -> *const c_void {
    crate::abort_on_panic(|| {
        if context.is_null() || out_status.is_null() {
            return std::ptr::null();
        }
        let Some(source) = (unsafe { source.units() }) else {
            unsafe { *out_status = FfiParseStatus::NotHandled };
            return std::ptr::null();
        };
        let outcome = match consume_a_list_of_component_values(&tokenize_for_parser(source)) {
            Ok(values) => parse_font_descriptor(unsafe { &*context }, kind, &values),
            Err(()) => ParseOutcome::Invalid,
        };
        match outcome {
            ParseOutcome::Parsed(value) => {
                unsafe { *out_status = FfiParseStatus::Parsed };
                Arc::into_raw(value).cast()
            }
            ParseOutcome::Invalid => {
                unsafe { *out_status = FfiParseStatus::Invalid };
                std::ptr::null()
            }
            ParseOutcome::NotHandled(_) => {
                unsafe { *out_status = FfiParseStatus::NotHandled };
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

    fn utf16(source: &str) -> Vec<u16> {
        source.encode_utf16().collect()
    }

    fn retained_utf16(source: &RetainedReadableString) -> Vec<u16> {
        let mut result = Vec::new();
        source.as_units().append_to(&mut result);
        result
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
            normalize_svg_path_data: None,
            font_format_is_supported: None,
            font_tech_is_supported: None,
            random_function_index: std::ptr::null_mut(),
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
        assert!(matches!(
            parse_css_value(&context(), u16::MAX, &values),
            ParseOutcome::NotHandled(_)
        ));
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
        assert!(matches!(
            parse(property_id::ALIGN_ITEMS, "normal"),
            ParseOutcome::NotHandled(_)
        ));
        assert!(matches!(
            parse(property_id::FONT_FAMILY, "\"Ladybird Sans\""),
            ParseOutcome::Parsed(_)
        ));
    }

    #[test]
    fn parses_color_properties() {
        for (property, source) in [
            (property_id::COLOR, "red"),
            (property_id::BACKGROUND_COLOR, "rgb(1 2 3 / 50%)"),
            (property_id::BORDER_TOP_COLOR, "color(display-p3 1 0 0)"),
            (property_id::TEXT_DECORATION_COLOR, "color-mix(in oklch, red, blue)"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }

        let mut quirks_context = context();
        quirks_context.in_quirks_mode = true;
        assert!(matches!(
            parse_with_context(&quirks_context, property_id::BACKGROUND_COLOR, "123"),
            ParseOutcome::Parsed(_)
        ));
        assert!(matches!(
            parse(property_id::BACKGROUND_COLOR, "123"),
            ParseOutcome::NotHandled(_)
        ));
    }

    #[test]
    fn parses_counter_quote_and_color_scheme_values() {
        for (property, source) in [
            (property_id::COUNTER_INCREMENT, "chapter 2 section"),
            (property_id::COUNTER_RESET, "reversed(chapter) 5 section"),
            (property_id::COUNTER_SET, "none"),
            (property_id::LIST_STYLE_TYPE, "upper-roman"),
            (property_id::LIST_STYLE_TYPE, "symbols(cyclic \"a\" \"b\")"),
            (property_id::QUOTES, "\"«\" \"»\" \"‹\" \"›\""),
            (property_id::COLOR_SCHEME, "only light dark custom"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }

        assert!(matches!(
            parse(property_id::QUOTES, "\"unpaired\""),
            ParseOutcome::Invalid
        ));
        assert!(matches!(
            parse(property_id::COLOR_SCHEME, "light only dark"),
            ParseOutcome::Invalid
        ));
        assert!(matches!(
            parse(property_id::COUNTER_INCREMENT, "chapter calc(sibling-count())"),
            ParseOutcome::Parsed(_)
        ));
        assert!(matches!(
            parse(property_id::COLOR_SCHEME, "var(--scheme)"),
            ParseOutcome::NotHandled(_)
        ));
    }

    #[test]
    fn parses_content_counters_and_cursors() {
        for (property, source) in [
            (
                property_id::CONTENT,
                "\"Chapter \" counter(chapter) / \"Chapter number\"",
            ),
            (property_id::CONTENT, "open-quote url(marker.svg) close-quote"),
            (property_id::CONTENT, "counters(section, \".\", upper-roman)"),
            (property_id::CURSOR, "url(cursor.svg) 4 5, pointer"),
            (property_id::CURSOR, "crosshair"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }

        assert!(matches!(
            parse(property_id::CONTENT, "normal \"suffix\""),
            ParseOutcome::Invalid
        ));
        assert!(matches!(
            parse(property_id::CURSOR, "url(cursor.svg), unknown-cursor"),
            ParseOutcome::Invalid
        ));
    }

    #[test]
    fn parses_timeline_functions_and_insets() {
        for (property, source) in [
            (
                property_id::ANIMATION_TIMELINE,
                "scroll(), view(inline 1px auto), --named",
            ),
            (property_id::ANIMATION_TIMELINE, "scroll(root x), view(auto auto)"),
            (property_id::VIEW_TIMELINE_INSET, "1px, auto 2%, calc(1px + 1%)"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        for (property, source) in [
            (property_id::ANIMATION_TIMELINE, "scroll(root self)"),
            (property_id::ANIMATION_TIMELINE, "scroll(nearest root)"),
            (property_id::ANIMATION_TIMELINE, "view(inline block)"),
            (property_id::ANIMATION_TIMELINE, "view(auto auto 1px)"),
            (property_id::VIEW_TIMELINE_INSET, "auto auto auto"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
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
            ParseOutcome::Parsed(_)
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
    fn parses_calculations_and_random() {
        for source in ["calc(1 / 2)", "progress(1, 0, 2)"] {
            let ParseOutcome::Parsed(value) = parse(property_id::OPACITY, source) else {
                panic!("math function should parse: {source}");
            };
            assert!(matches!(&*value, StyleValueData::OpacityValue { .. }));
        }
        let mut random_function_index = 0;
        let mut random_context = context();
        let property_context = FfiValueParsingContext {
            kind: FfiValueParsingContextKind::Property,
            value: property_id::OPACITY,
            secondary_value: 0,
            name: FfiUtf16View::default(),
            allowed_channels: 0,
        };
        random_context.value_contexts = &property_context;
        random_context.value_context_count = 1;
        random_context.random_function_index = &mut random_function_index;
        let ParseOutcome::Parsed(value) = parse_with_context(&random_context, property_id::OPACITY, "random(0, 1)")
        else {
            panic!("random should parse with property parser state");
        };
        assert!(matches!(&*value, StyleValueData::OpacityValue { .. }));
        assert_eq!(random_function_index, 1);

        assert!(matches!(
            parse_with_context(&random_context, property_id::OPACITY, "random(0, random(0, 1))"),
            ParseOutcome::Parsed(_)
        ));
        assert_eq!(random_function_index, 3);
    }

    #[test]
    fn parses_non_math_functions_inside_calculations() {
        for (property, source) in [
            (property_id::TOP, "calc(anchor(top) + 1px)"),
            (property_id::OPACITY, "calc(sibling-count() / 10)"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        assert!(matches!(
            parse(property_id::WIDTH, "calc(anchor-size(width) + 1px)"),
            ParseOutcome::NotHandled(_)
        ));
    }

    #[test]
    fn leaves_random_in_canvas_colors_with_cpp() {
        let mut random_function_index = 0;
        let value_contexts = [
            FfiValueParsingContext {
                kind: FfiValueParsingContextKind::Special,
                value: 0,
                secondary_value: 0,
                name: std::ptr::null(),
                name_length: 0,
                allowed_channels: 0,
            },
            FfiValueParsingContext {
                kind: FfiValueParsingContextKind::Property,
                value: property_id::COLOR,
                secondary_value: 0,
                name: FfiUtf16View::default(),
                allowed_channels: 0,
            },
        ];
        let mut canvas_context = context();
        canvas_context.value_contexts = value_contexts.as_ptr();
        canvas_context.value_context_count = value_contexts.len();
        canvas_context.random_function_index = &mut random_function_index;

        assert!(matches!(
            parse_with_context(
                &canvas_context,
                property_id::COLOR,
                "rgb(random(30, 10) random(60, 10) random(90, 10))"
            ),
            ParseOutcome::NotHandled(_)
        ));
        assert_eq!(random_function_index, 0);
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
        ] {
            let ParseOutcome::NotHandled(reason) = parse(property, source) else {
                panic!("deferred function should fall back: {source}");
            };
            assert_eq!(reason.label, expected_reason);
        }
        assert!(matches!(
            parse(property_id::Z_INDEX, "sibling-index()"),
            ParseOutcome::Parsed(_)
        ));
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
