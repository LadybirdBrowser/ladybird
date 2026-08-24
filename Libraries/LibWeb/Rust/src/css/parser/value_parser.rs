/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Parsed values use the thread-confined shared graph owned by the C++ style objects.
#![allow(clippy::arc_with_non_send_sync)]

use crate::css::css_enums::{
    background_box, display_inside, display_outside, keyword, keyword_from_ascii_case_insensitive,
    keyword_to_background_box, keyword_to_counter_style_name_keyword, keyword_to_cross_origin_modifier_value,
    keyword_to_cursor_predefined, keyword_to_display_inside, keyword_to_display_outside,
    keyword_to_referrer_policy_modifier_value, keyword_to_symbols_type, keyword_to_text_decoration_line,
    keyword_to_text_underline_position_horizontal, keyword_to_text_underline_position_vertical, repetition,
    symbols_type, text_underline_position_horizontal, text_underline_position_vertical,
};
use crate::css::css_pixels::CssPixels;
use crate::css::css_tokenizer::{
    CssNumberType, ParserTokenKind, TokenizerInput, tokenize_for_parser, tokenize_for_parser_without_source,
};
use crate::css::display::FfiDisplay;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::math_functions::math_function_from_name;
use crate::css::parser::arbitrary_substitution::{
    SubstitutionFunctionsPresence, collect_arbitrary_substitution_function_presence, declaration_value_is_valid,
    substitution_function_presence_bits,
};
use crate::css::parser::calc_parser::{CalcParseError, parse_a_calc_function_node};
use crate::css::parser::color_parser::{is_color_function_name, parse_color_value};
use crate::css::parser::component_value::{
    ComponentKind, ComponentValue, consume_a_list_of_component_values, consume_a_small_list_of_component_values,
};
use crate::css::parser::fonts_parser::parse_font_property;
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
    FIRST_SHORTHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, longhands_for_shorthand, property_accepted_keywords,
    property_accepted_value_types, property_accepts_only_keywords, property_custom_ident_blacklist,
    property_has_coordinating_list_multiplicity, property_has_hashless_hex_color_quirk,
    property_has_unitless_length_quirk, property_id, property_initial_value,
    property_is_positional_value_list_shorthand, property_is_shorthand, property_maximum_value_count,
    property_numeric_ranges, property_percentages_resolve_to, property_resolve_legacy_value_alias,
};
use crate::css::retained_fly_string::{RetainedUtf16FlyString, RetainedUtf16FlyStringList};
use crate::css::style_compute::{LENGTH_UNIT_NAMES, px_length_unit};
use crate::css::style_value::{
    RetainedByteList, RetainedCounterDefinition, RetainedCounterDefinitionList, RetainedNumericRangeList,
    RetainedPropertyIdList, RetainedReadableString, RetainedRequestUrlModifier, RetainedRequestUrlModifierList,
    RetainedString, RetainedStyleValueData, RetainedStyleValueDataList, StyleValueData,
};
use std::ffi::c_void;
use std::sync::{Arc, OnceLock};

include!(concat!(env!("OUT_DIR"), "/dimension_units_generated.rs"));

pub(crate) fn is_dimension_unit(unit: &[u16]) -> bool {
    LENGTH_UNIT_NAMES
        .iter()
        .chain(ANGLE_UNIT_NAMES.iter())
        .chain(FLEX_UNIT_NAMES.iter())
        .chain(FREQUENCY_UNIT_NAMES.iter())
        .chain(RESOLUTION_UNIT_NAMES.iter())
        .chain(TIME_UNIT_NAMES.iter())
        .any(|known_unit| equals_ascii_case_insensitive(unit, known_unit.as_bytes()))
}

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
const VALUE_TYPE_LENGTH_PERCENTAGE: u8 = 26;
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

/// The C++ value-parsing contexts which affect grammar decisions.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
#[allow(dead_code)]
pub enum FfiValueParsingContextKind {
    Property,
    Function,
    Descriptor,
    Special,
}

/// One entry in the C++ Parser's value-context stack.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiValueParsingContext {
    pub kind: FfiValueParsingContextKind,
    /// PropertyID, SpecialContext, or AtRuleID, depending on `kind`.
    pub value: u16,
    /// DescriptorID when `kind` is Descriptor.
    pub secondary_value: u16,
    /// Function name when `kind` is Function.
    pub name: FfiUtf16View,
}

/// Main-thread-normalized SVG path data available to a callback-free worker parse.
#[repr(C)]
pub struct FfiPrecomputedSvgPath {
    pub source: *const u16,
    pub source_length: usize,
    pub normalized: *const u16,
    pub normalized_length: usize,
}

/// Parser state required by CSS value parsing.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ParseContext {
    pub in_quirks_mode: bool,
    pub is_svg_presentation_attribute: bool,
    pub is_substituted_value: bool,
    pub contains_attr_tainted_values: bool,
    pub value_contexts: *const FfiValueParsingContext,
    pub value_context_count: usize,
    pub document_url: *const u8,
    pub document_url_length: usize,
    pub document_base_url: *const u8,
    pub document_base_url_length: usize,
    pub intern_utf16_fly_string: Option<unsafe extern "C" fn(*const u16, usize) -> usize>,
    pub normalize_svg_path_data: Option<unsafe extern "C" fn(*const u16, usize) -> usize>,
    pub precomputed_svg_paths: *const FfiPrecomputedSvgPath,
    pub precomputed_svg_path_count: usize,
    pub font_format_is_supported: Option<unsafe extern "C" fn(*const u16, usize) -> bool>,
    pub font_tech_is_supported: Option<unsafe extern "C" fn(u8) -> bool>,
    pub descriptor_integer_resolution_context: *const c_void,
    pub resolve_descriptor_integer: Option<unsafe extern "C" fn(*const c_void, *const c_void, *mut i32) -> bool>,
    pub random_function_index: *mut usize,
}

impl ParseContext {
    pub(crate) fn precomputed_svg_path(&self, source: &[u16]) -> Option<Option<&[u16]>> {
        if self.precomputed_svg_path_count == 0 {
            return None;
        }
        let paths = unsafe { std::slice::from_raw_parts(self.precomputed_svg_paths, self.precomputed_svg_path_count) };
        paths.iter().find_map(|path| {
            let candidate = unsafe { std::slice::from_raw_parts(path.source, path.source_length) };
            if candidate != source {
                return None;
            }
            if path.normalized.is_null() {
                return Some(None);
            }
            Some(Some(unsafe {
                std::slice::from_raw_parts(path.normalized, path.normalized_length)
            }))
        })
    }
}

pub(crate) enum ParseOutcome {
    Parsed(Arc<StyleValueData>),
    Invalid,
    NotHandled,
}

struct SharedKeywordValues(Box<[Arc<StyleValueData>]>);

// SAFETY: This cache only contains the pointer-free `Keyword` variant. Other
// `StyleValueData` variants remain thread-bound and are never inserted here.
unsafe impl Send for SharedKeywordValues {}
// SAFETY: The cached keyword values are immutable after initialization.
unsafe impl Sync for SharedKeywordValues {}

fn shared_style_value(value: StyleValueData) -> Arc<StyleValueData> {
    let StyleValueData::Keyword { keyword } = value else {
        return Arc::new(value);
    };

    static KEYWORD_VALUES: OnceLock<SharedKeywordValues> = OnceLock::new();
    let values = KEYWORD_VALUES.get_or_init(|| {
        SharedKeywordValues(
            (0..keyword::NAMES.len())
                .map(|keyword| {
                    Arc::new(StyleValueData::Keyword {
                        keyword: keyword as u16,
                    })
                })
                .collect(),
        )
    });
    values.0[usize::from(keyword)].clone()
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
    // NB: These properties have extra single-keyword grammar constraints or specialized value types.
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

pub(crate) fn parse_string_value(context: &ParseContext, value: &ComponentValue) -> Option<StyleValueData> {
    string_style_value(context, value.string()?)
}

pub(crate) fn parse_custom_ident_value(
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
    let parsed = if let Some(value) = parse_tree_counting_value(context, value, 0) {
        Some(value)
    } else if let Some((name, values)) = value.function()
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
    let parsed = if let Some(value) = parse_tree_counting_value(context, value, 1) {
        Some(value)
    } else if let Some((name, values)) = value.function()
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

fn parse_ratio_value_with_context(
    context: &ParseContext,
    property: u16,
    values: &[&ComponentValue],
) -> Option<StyleValueData> {
    let parse_component = |value: &ComponentValue| {
        if let Some((name, arguments)) = value.function()
            && math_function_from_name(name).is_some()
        {
            parse_calculated_numeric_value_with_ranges(
                context,
                property,
                VALUE_TYPE_NUMBER,
                None,
                NumericRange::NON_NEGATIVE,
                name,
                arguments,
            )
        } else {
            parse_number_value(value, NumericRange::NON_NEGATIVE)
        }
    };
    let numerator = parse_component(values.first()?)?;
    let denominator = match values {
        [_] => StyleValueData::Number { value: 1.0 },
        [_, slash, denominator] if slash.is_delim(b'/') => parse_component(denominator)?,
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

fn contains_unported_function(values: &[ComponentValue]) -> bool {
    values.iter().any(|value| match &value.kind {
        ComponentKind::Function { name, values } => {
            if is_arbitrary_substitution_function(name) {
                true
            } else if math_function_from_name(name).is_some()
                || equals_ascii_case_insensitive(name, b"sibling-count")
                || equals_ascii_case_insensitive(name, b"sibling-index")
                || is_color_function_name(name)
                || is_position_shape_function_name(name)
                || is_transform_effect_function_name(name)
                || is_image_function_name(name)
            {
                contains_unported_function(values)
            } else {
                // NB: Numeric grammars can accept functions such as anchor-size(). Until this
                //     parser handles a function itself, it cannot authoritatively reject the
                //     enclosing value.
                true
            }
        }
        ComponentKind::SimpleBlock { values, .. } => contains_unported_function(values),
        ComponentKind::Token(_) => false,
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
    if matches!(value_type, VALUE_TYPE_NUMBER | VALUE_TYPE_INTEGER)
        && let Some(value) = parse_tree_counting_value(context, value, u8::from(value_type == VALUE_TYPE_INTEGER))
    {
        return Some(value);
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

pub(crate) fn parse_syntax_numeric_value(
    context: &ParseContext,
    syntax_type: crate::css::parser::syntax::SyntaxType,
    value: &ComponentValue,
) -> Option<StyleValueData> {
    use crate::css::parser::syntax::SyntaxType;

    let value_type = match syntax_type {
        SyntaxType::Angle => VALUE_TYPE_ANGLE,
        SyntaxType::Integer => VALUE_TYPE_INTEGER,
        SyntaxType::Length | SyntaxType::LengthPercentage => VALUE_TYPE_LENGTH,
        SyntaxType::Number => VALUE_TYPE_NUMBER,
        SyntaxType::Percentage => VALUE_TYPE_PERCENTAGE,
        SyntaxType::Resolution => VALUE_TYPE_RESOLUTION,
        SyntaxType::Time => VALUE_TYPE_TIME,
        _ => return None,
    };
    if matches!(syntax_type, SyntaxType::Integer | SyntaxType::Number)
        && let Some(value) = parse_tree_counting_value(context, value, u8::from(syntax_type == SyntaxType::Integer))
    {
        return Some(value);
    }
    if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        return parse_calculated_numeric_value_with_ranges(
            context,
            property_id::CUSTOM,
            value_type,
            (syntax_type == SyntaxType::LengthPercentage).then_some(VALUE_TYPE_LENGTH),
            NumericRange::INFINITE,
            name,
            values,
        );
    }
    match syntax_type {
        SyntaxType::Angle => parse_angle_value(context, value, NumericRange::INFINITE),
        SyntaxType::Integer => parse_integer_value(value, NumericRange::INFINITE),
        SyntaxType::Length => parse_length_value(context, property_id::CUSTOM, value, NumericRange::INFINITE),
        SyntaxType::LengthPercentage => parse_length_percentage_value(
            context,
            property_id::CUSTOM,
            value,
            NumericRange::INFINITE,
            NumericRange::INFINITE,
        ),
        SyntaxType::Number => parse_number_value(value, NumericRange::INFINITE),
        SyntaxType::Percentage => parse_percentage_value(value, NumericRange::INFINITE),
        SyntaxType::Resolution => parse_resolution_value(value, NumericRange::INFINITE),
        SyntaxType::Time => parse_time_value(value, NumericRange::INFINITE),
        _ => None,
    }
}

pub(crate) fn parse_tree_counting_value(
    context: &ParseContext,
    value: &ComponentValue,
    computed_type: u8,
) -> Option<StyleValueData> {
    let (name, values) = value.function()?;
    if !values.iter().all(ComponentValue::is_whitespace) || !context_allows_tree_counting_functions(context) {
        return None;
    }
    let function = if equals_ascii_case_insensitive(name, b"sibling-count") {
        0
    } else if equals_ascii_case_insensitive(name, b"sibling-index") {
        1
    } else {
        return None;
    };
    Some(StyleValueData::TreeCountingFunction {
        function,
        computed_type,
    })
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

fn substitution_function_context_names(context: &ParseContext) -> Option<Vec<Vec<u16>>> {
    if context.value_context_count == 0 || context.value_contexts.is_null() {
        return None;
    }
    let contexts = unsafe { std::slice::from_raw_parts(context.value_contexts, context.value_context_count) };
    let names = contexts
        .iter()
        .filter(|value_context| value_context.kind == FfiValueParsingContextKind::Function)
        .map(|function_context| unsafe { function_context.name.to_utf16() })
        .collect::<Option<Vec<_>>>()?;
    (!names.is_empty()).then_some(names)
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

fn keyword_sequence(values: &[ComponentValue]) -> Option<Vec<u16>> {
    values
        .iter()
        .filter(|value| !value.is_whitespace())
        .map(|value| keyword_from_ascii_case_insensitive(value.ident()?))
        .collect()
}

fn keyword_value_list(keywords: Vec<u16>) -> StyleValueData {
    value_list(
        keywords
            .into_iter()
            .map(|keyword| StyleValueData::Keyword { keyword })
            .collect(),
        0,
        true,
    )
}

fn parse_keyword_combination_property(property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let Some(mut keywords) = keyword_sequence(values) else {
        return ParseOutcome::Invalid;
    };
    if keywords.is_empty() {
        return ParseOutcome::Invalid;
    }

    let parsed = match property {
        property_id::CONTAIN => {
            if keywords.len() == 1 && matches!(keywords[0], keyword::NONE | keyword::STRICT | keyword::CONTENT) {
                StyleValueData::Keyword { keyword: keywords[0] }
            } else {
                let order = [
                    keyword::SIZE,
                    keyword::INLINE_SIZE,
                    keyword::LAYOUT,
                    keyword::STYLE,
                    keyword::PAINT,
                ];
                let size_count = keywords
                    .iter()
                    .filter(|&&value| matches!(value, keyword::SIZE | keyword::INLINE_SIZE))
                    .count();
                if size_count > 1 || keywords.iter().any(|value| !order.contains(value)) {
                    return ParseOutcome::Invalid;
                }
                let original_length = keywords.len();
                keywords.sort_by_key(|value| order.iter().position(|candidate| candidate == value));
                keywords.dedup();
                if keywords.len() != original_length {
                    return ParseOutcome::Invalid;
                }
                keyword_value_list(keywords)
            }
        }
        property_id::CONTAINER_TYPE => {
            if keywords == [keyword::NORMAL] {
                StyleValueData::Keyword {
                    keyword: keyword::NORMAL,
                }
            } else {
                let order = [keyword::SIZE, keyword::INLINE_SIZE, keyword::SCROLL_STATE];
                let size_count = keywords
                    .iter()
                    .filter(|&&value| matches!(value, keyword::SIZE | keyword::INLINE_SIZE))
                    .count();
                if size_count > 1 || keywords.iter().any(|value| !order.contains(value)) {
                    return ParseOutcome::Invalid;
                }
                let original_length = keywords.len();
                keywords.sort_by_key(|value| order.iter().position(|candidate| candidate == value));
                keywords.dedup();
                if keywords.len() != original_length {
                    return ParseOutcome::Invalid;
                }
                keyword_value_list(keywords)
            }
        }
        property_id::POSITION_VISIBILITY => {
            if keywords == [keyword::ALWAYS] {
                StyleValueData::Keyword {
                    keyword: keyword::ALWAYS,
                }
            } else {
                let order = [keyword::ANCHORS_VALID, keyword::ANCHORS_VISIBLE, keyword::NO_OVERFLOW];
                if keywords.iter().any(|value| !order.contains(value)) {
                    return ParseOutcome::Invalid;
                }
                let original_length = keywords.len();
                keywords.sort_by_key(|value| order.iter().position(|candidate| candidate == value));
                keywords.dedup();
                if keywords.len() != original_length {
                    return ParseOutcome::Invalid;
                }
                keyword_value_list(keywords)
            }
        }
        property_id::TOUCH_ACTION => {
            if keywords.len() == 1 && matches!(keywords[0], keyword::AUTO | keyword::NONE | keyword::MANIPULATION) {
                StyleValueData::Keyword { keyword: keywords[0] }
            } else {
                let horizontal = [keyword::PAN_X, keyword::PAN_LEFT, keyword::PAN_RIGHT];
                let vertical = [keyword::PAN_Y, keyword::PAN_UP, keyword::PAN_DOWN];
                let horizontal_count = keywords.iter().filter(|value| horizontal.contains(value)).count();
                let vertical_count = keywords.iter().filter(|value| vertical.contains(value)).count();
                let pinch_count = keywords.iter().filter(|&&value| value == keyword::PINCH_ZOOM).count();
                if horizontal_count > 1
                    || vertical_count > 1
                    || pinch_count > 1
                    || keywords.iter().any(|value| {
                        !horizontal.contains(value) && !vertical.contains(value) && *value != keyword::PINCH_ZOOM
                    })
                {
                    return ParseOutcome::Invalid;
                }
                keywords.sort_by_key(|value| {
                    if horizontal.contains(value) {
                        0
                    } else if vertical.contains(value) {
                        1
                    } else {
                        2
                    }
                });
                keyword_value_list(keywords)
            }
        }
        property_id::WHITE_SPACE_TRIM => {
            if keywords == [keyword::NONE] {
                StyleValueData::Keyword { keyword: keyword::NONE }
            } else {
                let order = [keyword::DISCARD_BEFORE, keyword::DISCARD_AFTER, keyword::DISCARD_INNER];
                if keywords.iter().any(|value| !order.contains(value)) {
                    return ParseOutcome::Invalid;
                }
                let original_length = keywords.len();
                keywords.sort_by_key(|value| order.iter().position(|candidate| candidate == value));
                keywords.dedup();
                if keywords.len() != original_length {
                    return ParseOutcome::Invalid;
                }
                keyword_value_list(keywords)
            }
        }
        property_id::TEXT_DECORATION_LINE => {
            if keywords == [keyword::NONE] {
                StyleValueData::Keyword { keyword: keyword::NONE }
            } else {
                if keywords
                    .iter()
                    .any(|&value| value == keyword::NONE || keyword_to_text_decoration_line(value).is_none())
                    || (keywords.len() > 1
                        && keywords
                            .iter()
                            .any(|&value| matches!(value, keyword::SPELLING_ERROR | keyword::GRAMMAR_ERROR)))
                {
                    return ParseOutcome::Invalid;
                }
                let original_length = keywords.len();
                keywords.sort_by_key(|&value| keyword_to_text_decoration_line(value));
                keywords.dedup();
                if keywords.len() != original_length {
                    return ParseOutcome::Invalid;
                }
                keyword_value_list(keywords)
            }
        }
        _ => return ParseOutcome::NotHandled,
    };
    ParseOutcome::Parsed(shared_style_value(parsed))
}

fn parse_paint_order_property(values: &[ComponentValue]) -> ParseOutcome {
    let Some(keywords) = keyword_sequence(values) else {
        return ParseOutcome::Invalid;
    };
    if keywords == [keyword::NORMAL] {
        return ParseOutcome::Parsed(shared_style_value(StyleValueData::Keyword {
            keyword: keyword::NORMAL,
        }));
    }
    if keywords.is_empty()
        || keywords.len() > 3
        || keywords
            .iter()
            .any(|value| !matches!(*value, keyword::FILL | keyword::STROKE | keyword::MARKERS))
        || {
            let mut unique = keywords.clone();
            unique.sort_unstable();
            unique.dedup();
            unique.len() != keywords.len()
        }
    {
        return ParseOutcome::Invalid;
    }
    let first = keywords[0];
    if keywords.len() == 1
        || keywords[1]
            == if first == keyword::FILL {
                keyword::STROKE
            } else {
                keyword::FILL
            }
    {
        return ParseOutcome::Parsed(shared_style_value(StyleValueData::Keyword { keyword: first }));
    }
    ParseOutcome::Parsed(shared_style_value(keyword_value_list(keywords[..2].to_vec())))
}

fn parse_scrollbar_gutter_property(values: &[ComponentValue]) -> ParseOutcome {
    let Some(keywords) = keyword_sequence(values) else {
        return ParseOutcome::Invalid;
    };
    let value = match keywords.as_slice() {
        [keyword::AUTO] => 0,
        [keyword::STABLE] => 1,
        [keyword::STABLE, keyword::BOTH_EDGES] | [keyword::BOTH_EDGES, keyword::STABLE] => 2,
        _ => return ParseOutcome::Invalid,
    };
    ParseOutcome::Parsed(shared_style_value(StyleValueData::ScrollbarGutter { value }))
}

fn parse_aspect_ratio_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let non_whitespace = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    let auto_at_start = non_whitespace
        .first()
        .is_some_and(|value| parse_specific_keyword(value, &[keyword::AUTO]).is_some());
    let auto_at_end = non_whitespace
        .last()
        .is_some_and(|value| parse_specific_keyword(value, &[keyword::AUTO]).is_some());
    let ratio_values = if auto_at_start {
        &non_whitespace[1..]
    } else if auto_at_end {
        &non_whitespace[..non_whitespace.len() - 1]
    } else {
        non_whitespace.as_slice()
    };
    let ratio = (!ratio_values.is_empty())
        .then(|| parse_ratio_value_with_context(context, property, ratio_values))
        .flatten();
    match (auto_at_start || auto_at_end, ratio) {
        (true, Some(ratio)) => ParseOutcome::Parsed(shared_style_value(value_list(
            vec![StyleValueData::Keyword { keyword: keyword::AUTO }, ratio],
            0,
            true,
        ))),
        (true, None) if non_whitespace.len() == 1 => {
            ParseOutcome::Parsed(shared_style_value(StyleValueData::Keyword { keyword: keyword::AUTO }))
        }
        (false, Some(ratio)) => ParseOutcome::Parsed(shared_style_value(ratio)),
        _ => ParseOutcome::Invalid,
    }
}

fn parse_math_depth_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let Some(value) = single_non_whitespace_value(values) else {
        return ParseOutcome::Invalid;
    };
    if let Some(keyword) = parse_specific_keyword(value, &[keyword::AUTO_ADD]) {
        return ParseOutcome::Parsed(shared_style_value(keyword));
    }
    if let Some(integer) = parse_single_numeric_value_type(context, property, VALUE_TYPE_INTEGER, value) {
        return ParseOutcome::Parsed(shared_style_value(integer));
    }
    let Some((name, arguments)) = value.function() else {
        return ParseOutcome::Invalid;
    };
    if !equals_ascii_case_insensitive(name, b"add") {
        return ParseOutcome::Invalid;
    }
    let mut stream = TokenStream::new(arguments);
    let Some(integer) = parse_integer_from_stream(context, property, &mut stream, NumericRange::INFINITE) else {
        return ParseOutcome::Invalid;
    };
    stream.discard_whitespace();
    if stream.has_next_token() {
        return ParseOutcome::Invalid;
    }
    let Some(name) = retain_fly_string(context, &"add".encode_utf16().collect::<Vec<_>>()) else {
        return ParseOutcome::NotHandled;
    };
    ParseOutcome::Parsed(shared_style_value(StyleValueData::Function {
        name,
        value: RetainedStyleValueData::from_owned(integer),
    }))
}

fn parse_scrollbar_color_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if single_non_whitespace_value(values)
        .is_some_and(|value| parse_specific_keyword(value, &[keyword::AUTO]).is_some())
    {
        return ParseOutcome::Parsed(shared_style_value(StyleValueData::Keyword { keyword: keyword::AUTO }));
    }
    let mut stream = TokenStream::new(values);
    stream.discard_whitespace();
    let Some(thumb_color) = parse_color_value(context, property, &mut stream, false) else {
        return ParseOutcome::Invalid;
    };
    stream.discard_whitespace();
    let Some(track_color) = parse_color_value(context, property, &mut stream, false) else {
        return ParseOutcome::Invalid;
    };
    stream.discard_whitespace();
    if stream.has_next_token() {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::Parsed(shared_style_value(StyleValueData::ScrollbarColor {
        thumb_color: RetainedStyleValueData::from_owned(thumb_color),
        track_color: RetainedStyleValueData::from_owned(track_color),
    }))
}

fn parse_stroke_dasharray_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if single_non_whitespace_value(values)
        .is_some_and(|value| parse_specific_keyword(value, &[keyword::NONE]).is_some())
    {
        return ParseOutcome::Parsed(shared_style_value(StyleValueData::Keyword { keyword: keyword::NONE }));
    }
    let mut stream = TokenStream::new(values);
    let mut dashes = Vec::new();
    while stream.has_next_token() {
        stream.discard_whitespace();
        let parsed =
            parse_number_from_stream(context, property, &mut stream, NumericRange::NON_NEGATIVE).or_else(|| {
                parse_length_percentage_from_stream(
                    context,
                    property,
                    &mut stream,
                    NumericRange::NON_NEGATIVE,
                    NumericRange::NON_NEGATIVE,
                )
            });
        let Some(parsed) = parsed else {
            return ParseOutcome::Invalid;
        };
        dashes.push(parsed);
        stream.discard_whitespace();
        if stream.has_next_token() && stream.next_token().is_comma() {
            stream.discard_a_token();
        }
    }
    if dashes.is_empty() {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::Parsed(shared_style_value(value_list(dashes, 1, true)))
}

fn parse_text_indent_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let mut stream = TokenStream::new(values);
    let mut length_percentage = None;
    let mut hanging = false;
    let mut each_line = false;
    while stream.has_next_token() {
        stream.discard_whitespace();
        if length_percentage.is_none()
            && let Some(parsed) = parse_length_percentage_from_stream(
                context,
                property,
                &mut stream,
                NumericRange::INFINITE,
                NumericRange::INFINITE,
            )
        {
            length_percentage = Some(parsed);
            continue;
        }
        let Some(keyword) = keyword_from_ascii_case_insensitive(stream.next_token().ident().unwrap_or_default()) else {
            return ParseOutcome::Invalid;
        };
        stream.discard_a_token();
        match keyword {
            keyword::HANGING if !hanging => hanging = true,
            keyword::EACH_LINE if !each_line => each_line = true,
            _ => return ParseOutcome::Invalid,
        }
    }
    let Some(length_percentage) = length_percentage else {
        return ParseOutcome::Invalid;
    };
    ParseOutcome::Parsed(shared_style_value(StyleValueData::TextIndent {
        length_percentage: RetainedStyleValueData::from_owned(length_percentage),
        hanging,
        each_line,
    }))
}

fn parse_text_underline_position_property(values: &[ComponentValue]) -> ParseOutcome {
    let Some(keywords) = keyword_sequence(values) else {
        return ParseOutcome::Invalid;
    };
    if keywords == [keyword::AUTO] {
        return ParseOutcome::Parsed(shared_style_value(StyleValueData::TextUnderlinePosition {
            horizontal: text_underline_position_horizontal::AUTO,
            vertical: text_underline_position_vertical::AUTO,
        }));
    }
    let mut horizontal = None;
    let mut vertical = None;
    for keyword in keywords {
        if let Some(value) = keyword_to_text_underline_position_horizontal(keyword) {
            if value == text_underline_position_horizontal::AUTO || horizontal.replace(value).is_some() {
                return ParseOutcome::Invalid;
            }
        } else if let Some(value) = keyword_to_text_underline_position_vertical(keyword) {
            if value == text_underline_position_vertical::AUTO || vertical.replace(value).is_some() {
                return ParseOutcome::Invalid;
            }
        } else {
            return ParseOutcome::Invalid;
        }
    }
    ParseOutcome::Parsed(shared_style_value(StyleValueData::TextUnderlinePosition {
        horizontal: horizontal.unwrap_or(text_underline_position_horizontal::AUTO),
        vertical: vertical.unwrap_or(text_underline_position_vertical::AUTO),
    }))
}

fn parse_overflow_clip_margin_property(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    let mut stream = TokenStream::new(values);
    let mut visual_box = None;
    let mut length = None;
    for _ in 0..2 {
        stream.discard_whitespace();
        if !stream.has_next_token() {
            break;
        }
        if visual_box.is_none()
            && let Some(keyword) = stream
                .next_token()
                .ident()
                .and_then(keyword_from_ascii_case_insensitive)
            && let Some(box_value) = keyword_to_background_box(keyword)
            && matches!(
                box_value,
                background_box::CONTENT_BOX | background_box::PADDING_BOX | background_box::BORDER_BOX
            )
        {
            visual_box = Some(box_value);
            stream.discard_a_token();
            continue;
        }
        if length.is_none()
            && let Some(parsed) = parse_length_from_stream(context, property, &mut stream, NumericRange::NON_NEGATIVE)
        {
            length = Some(parsed);
            continue;
        }
        return ParseOutcome::Invalid;
    }
    stream.discard_whitespace();
    if stream.has_next_token() || (visual_box.is_none() && length.is_none()) {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::Parsed(shared_style_value(StyleValueData::OverflowClipMargin {
        has_visual_box: visual_box.is_some(),
        visual_box: visual_box.unwrap_or(0),
        offset: RetainedStyleValueData::from_owned(length.unwrap_or(StyleValueData::Length {
            value: 0.0,
            unit: px_length_unit(),
        })),
    }))
}

fn parse_grid_auto_flow_property(values: &[ComponentValue]) -> ParseOutcome {
    let mut row = true;
    let mut has_axis = false;
    let mut dense = false;
    for value in values.iter().filter(|value| !value.is_whitespace()) {
        let Some(identifier) = value.ident() else {
            return ParseOutcome::Invalid;
        };
        if equals_ascii_case_insensitive(identifier, b"row") && !has_axis {
            has_axis = true;
        } else if equals_ascii_case_insensitive(identifier, b"column") && !has_axis {
            has_axis = true;
            row = false;
        } else if equals_ascii_case_insensitive(identifier, b"dense") && !dense {
            dense = true;
        } else {
            return ParseOutcome::Invalid;
        }
    }
    if !has_axis && !dense {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::Parsed(shared_style_value(StyleValueData::GridAutoFlow { row, dense }))
}

fn parse_alignment_property(property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let Some(keyword) = single_non_whitespace_value(values)
        .and_then(ComponentValue::ident)
        .and_then(keyword_from_ascii_case_insensitive)
    else {
        return ParseOutcome::Invalid;
    };
    if matches!(keyword, keyword::SAFE | keyword::UNSAFE)
        || property_accepted_keywords(property).binary_search(&keyword).is_err()
    {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::Parsed(shared_style_value(StyleValueData::Keyword { keyword }))
}

fn parse_paint_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if single_non_whitespace_value(values)
        .and_then(ComponentValue::ident)
        .and_then(keyword_from_ascii_case_insensitive)
        .is_some_and(|keyword| keyword == keyword::NONE)
    {
        let keyword =
            keyword_from_ascii_case_insensitive(single_non_whitespace_value(values).unwrap().ident().unwrap()).unwrap();
        return ParseOutcome::Parsed(shared_style_value(StyleValueData::Keyword { keyword }));
    }
    let mut stream = TokenStream::new(values);
    if let Some(color) = parse_color_value(context, property, &mut stream, false) {
        stream.discard_whitespace();
        if !stream.has_next_token() {
            return ParseOutcome::Parsed(shared_style_value(color));
        }
    }

    let mut stream = TokenStream::new(values);
    stream.discard_whitespace();
    let Some(url) = parse_url_value(context, stream.next_token()) else {
        return ParseOutcome::Invalid;
    };
    stream.discard_a_token();
    stream.discard_whitespace();
    let fallback = if !stream.has_next_token() {
        None
    } else if stream
        .next_token()
        .ident()
        .and_then(keyword_from_ascii_case_insensitive)
        .is_some_and(|keyword| keyword == keyword::NONE)
    {
        stream.discard_a_token();
        Some(StyleValueData::Keyword { keyword: keyword::NONE })
    } else {
        parse_color_value(context, property, &mut stream, false)
    };
    stream.discard_whitespace();
    if stream.has_next_token() {
        return ParseOutcome::Invalid;
    }
    let parsed = vec![url, fallback.unwrap_or(StyleValueData::EmptyOptional)];
    ParseOutcome::Parsed(shared_style_value(value_list(parsed, 0, true)))
}

fn parse_repeat_item(values: &[ComponentValue]) -> Option<StyleValueData> {
    let identifiers = values
        .iter()
        .filter(|value| !value.is_whitespace())
        .map(ComponentValue::ident)
        .collect::<Option<Vec<_>>>()?;
    let repetition = |identifier: &[u16]| {
        repetition::NAMES
            .iter()
            .position(|name| equals_ascii_case_insensitive(identifier, name.as_bytes()))
            .and_then(|value| u8::try_from(value).ok())
    };
    match identifiers.as_slice() {
        [value] if equals_ascii_case_insensitive(value, b"repeat-x") => Some(StyleValueData::RepeatStyle {
            repeat_x: repetition::REPEAT,
            repeat_y: repetition::NO_REPEAT,
        }),
        [value] if equals_ascii_case_insensitive(value, b"repeat-y") => Some(StyleValueData::RepeatStyle {
            repeat_x: repetition::NO_REPEAT,
            repeat_y: repetition::REPEAT,
        }),
        [x] => {
            let x = repetition(x)?;
            Some(StyleValueData::RepeatStyle {
                repeat_x: x,
                repeat_y: x,
            })
        }
        [x, y] => Some(StyleValueData::RepeatStyle {
            repeat_x: repetition(x)?,
            repeat_y: repetition(y)?,
        }),
        _ => None,
    }
}

fn parse_repeat_property(values: &[ComponentValue]) -> ParseOutcome {
    let parsed = values
        .split(ComponentValue::is_comma)
        .map(parse_repeat_item)
        .collect::<Option<Vec<_>>>();
    let Some(parsed) = parsed else {
        return ParseOutcome::Invalid;
    };
    ParseOutcome::Parsed(shared_style_value(value_list(parsed, 1, true)))
}

fn parse_background_size_item(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let non_whitespace = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    if non_whitespace.len() == 1
        && let Some(keyword) = non_whitespace[0].ident().and_then(keyword_from_ascii_case_insensitive)
        && matches!(keyword, keyword::COVER | keyword::CONTAIN)
    {
        return Some(StyleValueData::Keyword { keyword });
    }
    if non_whitespace.is_empty() || non_whitespace.len() > 2 {
        return None;
    }
    let parse_size = |value: &ComponentValue| {
        parse_specific_keyword(value, &[keyword::AUTO]).or_else(|| {
            if let Some((name, arguments)) = value.function()
                && math_function_from_name(name).is_some()
            {
                parse_calculated_numeric_value_with_ranges(
                    context,
                    property,
                    VALUE_TYPE_LENGTH,
                    Some(VALUE_TYPE_LENGTH),
                    NumericRange::NON_NEGATIVE,
                    name,
                    arguments,
                )
            } else {
                parse_length_percentage_value(
                    context,
                    property,
                    value,
                    NumericRange::NON_NEGATIVE,
                    NumericRange::NON_NEGATIVE,
                )
            }
        })
    };
    let size_x = parse_size(non_whitespace[0])?;
    let size_y = if non_whitespace.len() == 2 {
        parse_size(non_whitespace[1])?
    } else {
        StyleValueData::Keyword { keyword: keyword::AUTO }
    };
    Some(StyleValueData::BackgroundSize {
        size_x: RetainedStyleValueData::from_owned(size_x),
        size_y: RetainedStyleValueData::from_owned(size_y),
    })
}

fn parse_background_size_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let parsed = values
        .split(ComponentValue::is_comma)
        .map(|item| parse_background_size_item(context, property, item))
        .collect::<Option<Vec<_>>>();
    let Some(parsed) = parsed else {
        return ParseOutcome::Invalid;
    };
    ParseOutcome::Parsed(shared_style_value(value_list(parsed, 1, true)))
}

fn parse_border_image_slice_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let mut slices = Vec::new();
    let mut fill = false;
    let significant = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    for (index, value) in significant.iter().enumerate() {
        if value
            .ident()
            .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"fill"))
        {
            if fill || (index != 0 && index + 1 != significant.len()) {
                return ParseOutcome::Invalid;
            }
            fill = true;
            continue;
        }
        if slices.len() == 4 {
            return ParseOutcome::Invalid;
        }
        let parsed = if let Some((name, arguments)) = value.function()
            && math_function_from_name(name).is_some()
        {
            parse_calculated_numeric_value_with_ranges(
                context,
                property,
                VALUE_TYPE_NUMBER,
                None,
                NumericRange::NON_NEGATIVE,
                name,
                arguments,
            )
            .or_else(|| {
                parse_calculated_numeric_value_with_ranges(
                    context,
                    property,
                    VALUE_TYPE_PERCENTAGE,
                    None,
                    NumericRange::NON_NEGATIVE,
                    name,
                    arguments,
                )
            })
        } else {
            parse_number_percentage_value(value, NumericRange::NON_NEGATIVE, NumericRange::NON_NEGATIVE)
        };
        let Some(parsed) = parsed else {
            return ParseOutcome::Invalid;
        };
        slices.push(parsed);
    }
    let (top, right, bottom, left) = match slices.as_slice() {
        [all] => (all, all, all, all),
        [vertical, horizontal] => (vertical, horizontal, vertical, horizontal),
        [top, horizontal, bottom] => (top, horizontal, bottom, horizontal),
        [top, right, bottom, left] => (top, right, bottom, left),
        _ => return ParseOutcome::Invalid,
    };
    ParseOutcome::Parsed(shared_style_value(StyleValueData::BorderImageSlice {
        top: RetainedStyleValueData::from_owned(top.clone()),
        right: RetainedStyleValueData::from_owned(right.clone()),
        bottom: RetainedStyleValueData::from_owned(bottom.clone()),
        left: RetainedStyleValueData::from_owned(left.clone()),
        fill,
    }))
}

fn parse_shadow_item(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
    is_text_shadow: bool,
) -> Option<StyleValueData> {
    let mut stream = TokenStream::new(values);
    stream.discard_whitespace();
    let mut color = None;
    let mut offset_x = None;
    let mut offset_y = None;
    let mut blur_radius = None;
    let mut spread_distance = None;
    let mut inset = false;

    while stream.has_next_token() {
        if let Some(parsed_color) = parse_color_value(context, property, &mut stream, false) {
            if color.is_some() {
                return None;
            }
            color = Some(parsed_color);
            stream.discard_whitespace();
            continue;
        }

        if !is_text_shadow
            && !inset
            && stream
                .next_token()
                .ident()
                .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"inset"))
        {
            inset = true;
            stream.discard_a_token();
            stream.discard_whitespace();
            continue;
        }

        if offset_x.is_none() {
            let parsed_offset_x = parse_length_from_stream(context, property, &mut stream, NumericRange::INFINITE)?;
            stream.discard_whitespace();
            if !stream.has_next_token() {
                return None;
            }
            let parsed_offset_y = parse_length_from_stream(context, property, &mut stream, NumericRange::INFINITE)?;
            offset_x = Some(parsed_offset_x);
            offset_y = Some(parsed_offset_y);
            stream.discard_whitespace();

            if stream.has_next_token()
                && let Some(parsed_blur_radius) =
                    parse_length_from_stream(context, property, &mut stream, NumericRange::NON_NEGATIVE)
            {
                blur_radius = Some(parsed_blur_radius);
                stream.discard_whitespace();
                if !is_text_shadow
                    && stream.has_next_token()
                    && let Some(parsed_spread_distance) =
                        parse_length_from_stream(context, property, &mut stream, NumericRange::INFINITE)
                {
                    spread_distance = Some(parsed_spread_distance);
                    stream.discard_whitespace();
                }
            }
            continue;
        }

        return None;
    }

    Some(StyleValueData::Shadow {
        shadow_type: u8::from(is_text_shadow),
        color: color.map_or_else(RetainedStyleValueData::none, RetainedStyleValueData::from_owned),
        offset_x: RetainedStyleValueData::from_owned(offset_x?),
        offset_y: RetainedStyleValueData::from_owned(offset_y?),
        blur_radius: blur_radius.map_or_else(RetainedStyleValueData::none, RetainedStyleValueData::from_owned),
        spread_distance: spread_distance.map_or_else(RetainedStyleValueData::none, RetainedStyleValueData::from_owned),
        placement: u8::from(inset),
    })
}

fn parse_shadow_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if single_non_whitespace_value(values)
        .and_then(ComponentValue::ident)
        .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"none"))
    {
        return ParseOutcome::Parsed(shared_style_value(StyleValueData::Keyword { keyword: keyword::NONE }));
    }
    let is_text_shadow = property == property_id::TEXT_SHADOW;
    let parsed = values
        .split(ComponentValue::is_comma)
        .map(|item| parse_shadow_item(context, property, item, is_text_shadow))
        .collect::<Option<Vec<_>>>();
    parsed.map_or(ParseOutcome::Invalid, |parsed| {
        ParseOutcome::Parsed(shared_style_value(value_list(parsed, 1, true)))
    })
}

fn parse_position_area_keywords(keywords: &[u16]) -> Option<StyleValueData> {
    let x = [
        keyword::LEFT,
        keyword::CENTER,
        keyword::RIGHT,
        keyword::SPAN_LEFT,
        keyword::SPAN_RIGHT,
        keyword::X_START,
        keyword::X_END,
        keyword::SPAN_X_START,
        keyword::SPAN_X_END,
        keyword::SELF_X_START,
        keyword::SELF_X_END,
        keyword::SPAN_SELF_X_START,
        keyword::SPAN_SELF_X_END,
        keyword::SPAN_ALL,
    ];
    let y = [
        keyword::TOP,
        keyword::CENTER,
        keyword::BOTTOM,
        keyword::SPAN_TOP,
        keyword::SPAN_BOTTOM,
        keyword::Y_START,
        keyword::Y_END,
        keyword::SPAN_Y_START,
        keyword::SPAN_Y_END,
        keyword::SELF_Y_START,
        keyword::SELF_Y_END,
        keyword::SPAN_SELF_Y_START,
        keyword::SPAN_SELF_Y_END,
        keyword::SPAN_ALL,
    ];
    let block = [
        keyword::BLOCK_START,
        keyword::CENTER,
        keyword::BLOCK_END,
        keyword::SPAN_BLOCK_START,
        keyword::SPAN_BLOCK_END,
        keyword::SPAN_ALL,
    ];
    let inline = [
        keyword::INLINE_START,
        keyword::CENTER,
        keyword::INLINE_END,
        keyword::SPAN_INLINE_START,
        keyword::SPAN_INLINE_END,
        keyword::SPAN_ALL,
    ];
    let self_block = [
        keyword::SELF_BLOCK_START,
        keyword::CENTER,
        keyword::SELF_BLOCK_END,
        keyword::SPAN_SELF_BLOCK_START,
        keyword::SPAN_SELF_BLOCK_END,
        keyword::SPAN_ALL,
    ];
    let self_inline = [
        keyword::SELF_INLINE_START,
        keyword::CENTER,
        keyword::SELF_INLINE_END,
        keyword::SPAN_SELF_INLINE_START,
        keyword::SPAN_SELF_INLINE_END,
        keyword::SPAN_ALL,
    ];
    let start_end = [
        keyword::START,
        keyword::CENTER,
        keyword::END,
        keyword::SPAN_START,
        keyword::SPAN_END,
        keyword::SPAN_ALL,
    ];
    let self_start_end = [
        keyword::SELF_START,
        keyword::CENTER,
        keyword::SELF_END,
        keyword::SPAN_SELF_START,
        keyword::SPAN_SELF_END,
        keyword::SPAN_ALL,
    ];
    let categories = [
        (&x[..], &y[..]),
        (&block[..], &inline[..]),
        (&self_block[..], &self_inline[..]),
    ];
    let ambiguous = [
        keyword::CENTER,
        keyword::SPAN_ALL,
        keyword::START,
        keyword::END,
        keyword::SELF_START,
        keyword::SELF_END,
        keyword::SPAN_START,
        keyword::SPAN_END,
        keyword::SPAN_SELF_START,
        keyword::SPAN_SELF_END,
    ];

    if let [keyword] = keywords {
        let accepted = categories
            .iter()
            .any(|(first, second)| first.contains(keyword) || second.contains(keyword))
            || start_end.contains(keyword)
            || self_start_end.contains(keyword);
        return accepted.then_some(StyleValueData::Keyword { keyword: *keyword });
    }
    let [first, second] = keywords else {
        return None;
    };
    let canonical = categories
        .iter()
        .find_map(|(first_axis, second_axis)| {
            if first_axis.contains(first) && second_axis.contains(second) {
                Some((*first, *second))
            } else if second_axis.contains(first) && first_axis.contains(second) {
                Some((*second, *first))
            } else {
                None
            }
        })
        .or_else(|| {
            (start_end.contains(first) && start_end.contains(second)
                || self_start_end.contains(first) && self_start_end.contains(second))
            .then_some((*first, *second))
        })?;
    if !ambiguous.contains(&canonical.0) && canonical.1 == keyword::SPAN_ALL {
        return Some(StyleValueData::Keyword { keyword: canonical.0 });
    }
    if !ambiguous.contains(&canonical.1) && canonical.0 == keyword::SPAN_ALL {
        return Some(StyleValueData::Keyword { keyword: canonical.1 });
    }
    Some(keyword_value_list(vec![canonical.0, canonical.1]))
}

fn parse_position_area_property(values: &[ComponentValue]) -> ParseOutcome {
    let Some(keywords) = keyword_sequence(values) else {
        return ParseOutcome::Invalid;
    };
    if keywords == [keyword::NONE] {
        return ParseOutcome::Parsed(shared_style_value(StyleValueData::Keyword { keyword: keyword::NONE }));
    }
    parse_position_area_keywords(&keywords).map_or(ParseOutcome::Invalid, |value| {
        ParseOutcome::Parsed(shared_style_value(value))
    })
}

fn parse_position_try_fallbacks_property(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    if keyword_sequence(values).is_some_and(|keywords| keywords == [keyword::NONE]) {
        return ParseOutcome::Parsed(shared_style_value(StyleValueData::Keyword { keyword: keyword::NONE }));
    }
    let parsed = values
        .split(ComponentValue::is_comma)
        .map(|item| {
            if let Some(keywords) = keyword_sequence(item)
                && let Some(position_area) = parse_position_area_keywords(&keywords)
            {
                return Some(position_area);
            }
            let mut dashed_ident = None;
            let mut tactics = Vec::new();
            let mut has_tactic_before_ident = false;
            let mut has_tactic_after_ident = false;
            for value in item.iter().filter(|value| !value.is_whitespace()) {
                if let Some(keyword) = value.ident().and_then(keyword_from_ascii_case_insensitive)
                    && matches!(
                        keyword,
                        keyword::FLIP_BLOCK | keyword::FLIP_INLINE | keyword::FLIP_START
                    )
                {
                    if dashed_ident.is_some() {
                        has_tactic_after_ident = true;
                    } else {
                        has_tactic_before_ident = true;
                    }
                    if has_tactic_before_ident && has_tactic_after_ident {
                        return None;
                    }
                    if tactics.iter().any(|existing| {
                        matches!(existing, StyleValueData::Keyword { keyword: existing } if *existing == keyword)
                    }) {
                        return None;
                    }
                    tactics.push(StyleValueData::Keyword { keyword });
                } else if dashed_ident.is_none() {
                    dashed_ident = Some(parse_dashed_ident_value(context, value)?);
                } else {
                    return None;
                }
            }
            let mut parts = Vec::new();
            if let Some(dashed_ident) = dashed_ident {
                parts.push(dashed_ident);
            }
            if !tactics.is_empty() {
                parts.push(value_list(tactics, 0, true));
            }
            (!parts.is_empty()).then(|| value_list(parts, 0, true))
        })
        .collect::<Option<Vec<_>>>();
    parsed.map_or(ParseOutcome::Invalid, |parsed| {
        ParseOutcome::Parsed(shared_style_value(value_list(parsed, 1, true)))
    })
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum TransformOriginAxis {
    None,
    X,
    Y,
}

struct TransformOriginOffset {
    axis: TransformOriginAxis,
    value: StyleValueData,
    is_plain_numeric: bool,
}

fn parse_transform_origin_offset(
    context: &ParseContext,
    property: u16,
    value: &ComponentValue,
) -> Option<TransformOriginOffset> {
    if let Some(keyword) = value.ident().and_then(keyword_from_ascii_case_insensitive) {
        let axis = match keyword {
            keyword::LEFT | keyword::RIGHT => TransformOriginAxis::X,
            keyword::TOP | keyword::BOTTOM => TransformOriginAxis::Y,
            keyword::CENTER => TransformOriginAxis::None,
            _ => return None,
        };
        return Some(TransformOriginOffset {
            axis,
            value: StyleValueData::Keyword { keyword },
            is_plain_numeric: false,
        });
    }
    if let Some(value) =
        parse_length_percentage_value(context, property, value, NumericRange::INFINITE, NumericRange::INFINITE)
    {
        return Some(TransformOriginOffset {
            axis: TransformOriginAxis::None,
            value,
            is_plain_numeric: true,
        });
    }
    let (name, arguments) = value.function()?;
    math_function_from_name(name)?;
    Some(TransformOriginOffset {
        axis: TransformOriginAxis::None,
        value: parse_calculated_numeric_value_with_ranges(
            context,
            property,
            VALUE_TYPE_LENGTH,
            Some(VALUE_TYPE_LENGTH),
            NumericRange::INFINITE,
            name,
            arguments,
        )?,
        is_plain_numeric: false,
    })
}

fn parse_transform_origin_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let significant = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    if significant.is_empty() || significant.len() > 3 {
        return ParseOutcome::Invalid;
    }
    let Some(first) = parse_transform_origin_offset(context, property, significant[0]) else {
        return ParseOutcome::Invalid;
    };
    let zero = || StyleValueData::Length {
        value: 0.0,
        unit: px_length_unit(),
    };
    if significant.len() == 1 {
        let (x, y) = if first.axis == TransformOriginAxis::Y {
            (
                StyleValueData::Keyword {
                    keyword: keyword::CENTER,
                },
                first.value,
            )
        } else {
            (
                first.value,
                StyleValueData::Keyword {
                    keyword: keyword::CENTER,
                },
            )
        };
        return ParseOutcome::Parsed(shared_style_value(value_list(vec![x, y, zero()], 0, true)));
    }
    let Some(second) = parse_transform_origin_offset(context, property, significant[1]) else {
        return ParseOutcome::Invalid;
    };
    if (first.is_plain_numeric && second.axis == TransformOriginAxis::X)
        || (second.is_plain_numeric && first.axis == TransformOriginAxis::Y)
        || (first.axis == TransformOriginAxis::X && second.axis == TransformOriginAxis::X)
        || (first.axis == TransformOriginAxis::Y && second.axis == TransformOriginAxis::Y)
    {
        return ParseOutcome::Invalid;
    }
    let z = if significant.len() == 3 {
        let value = significant[2];
        let parsed = if let Some((name, arguments)) = value.function()
            && math_function_from_name(name).is_some()
        {
            parse_calculated_numeric_value_with_ranges(
                context,
                property,
                VALUE_TYPE_LENGTH,
                None,
                NumericRange::INFINITE,
                name,
                arguments,
            )
        } else {
            parse_length_value(context, property, value, NumericRange::INFINITE)
        };
        let Some(parsed) = parsed else {
            return ParseOutcome::Invalid;
        };
        parsed
    } else {
        zero()
    };
    let (x, y) = match (first.axis, second.axis) {
        (TransformOriginAxis::X, _) => (first.value, second.value),
        (TransformOriginAxis::Y, _) => (second.value, first.value),
        (TransformOriginAxis::None, TransformOriginAxis::X) => (second.value, first.value),
        _ => (first.value, second.value),
    };
    ParseOutcome::Parsed(shared_style_value(value_list(vec![x, y, z], 0, true)))
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
        return ParseOutcome::NotHandled;
    }
    if contains_unported_function(values) {
        return ParseOutcome::NotHandled;
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
    parsed.map_or(ParseOutcome::Invalid, |parsed| {
        ParseOutcome::Parsed(shared_style_value(parsed))
    })
}

fn parse_color_scheme_property(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let non_whitespace = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    if non_whitespace.len() == 1
        && non_whitespace[0]
            .ident()
            .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"normal"))
    {
        return ParseOutcome::Parsed(shared_style_value(StyleValueData::ColorScheme {
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
        let Some(scheme) = retain_fly_string(context, identifier) else {
            return ParseOutcome::NotHandled;
        };
        schemes.push(scheme);
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
    ParseOutcome::Parsed(shared_style_value(StyleValueData::ColorScheme {
        schemes: RetainedUtf16FlyStringList::from_retained_strings(schemes),
        scheme_codes: RetainedByteList::from_bytes(scheme_codes),
        only,
    }))
}

fn parse_quotes_property(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    if let Some(value) = single_non_whitespace_value(values)
        && let Some(keyword) = parse_specific_keyword(value, &[keyword::AUTO, keyword::NONE])
    {
        return ParseOutcome::Parsed(shared_style_value(keyword));
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
    ParseOutcome::Parsed(shared_style_value(value_list(strings, 0, false)))
}

fn parse_counter_definitions_property(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    if let Some(value) = single_non_whitespace_value(values)
        && let Some(none) = parse_specific_keyword(value, &[keyword::NONE])
    {
        return ParseOutcome::Parsed(shared_style_value(none));
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
            let Some(name) = retain_fly_string(context, identifier) else {
                return ParseOutcome::NotHandled;
            };
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
            let Some(name) = retain_fly_string(context, identifier) else {
                return ParseOutcome::NotHandled;
            };
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
    ParseOutcome::Parsed(shared_style_value(StyleValueData::CounterDefinitions {
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
    parsed.map_or(ParseOutcome::Invalid, |parsed| {
        ParseOutcome::Parsed(shared_style_value(parsed))
    })
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
    if let Some(value) = single_non_whitespace_value(values)
        && let Some(keyword) = parse_specific_keyword(value, &[keyword::NONE, keyword::NORMAL])
    {
        return ParseOutcome::Parsed(shared_style_value(keyword));
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
    ParseOutcome::Parsed(shared_style_value(StyleValueData::Content {
        content: RetainedStyleValueData::from_owned(value_list(content, 0, true)),
        alt_text: if alt_text.is_empty() {
            RetainedStyleValueData::none()
        } else {
            RetainedStyleValueData::from_owned(value_list(alt_text, 0, true))
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
    ParseOutcome::Parsed(shared_style_value(match cursors.as_slice() {
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
        ParseOutcome::Parsed(shared_style_value(value_list(timelines, 1, true)))
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
        ParseOutcome::Parsed(shared_style_value(value_list(insets, 1, true)))
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
            }) && contains_unported_function(values)
                && !contains_tree_counting_function(values)
            {
                return ParseOutcome::NotHandled;
            }
            parse_counter_definitions_property(context, property, values)
        }
        _ => ParseOutcome::NotHandled,
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
        return ParseOutcome::NotHandled;
    }
    if contains_unported_function(values) {
        return ParseOutcome::NotHandled;
    }

    let fully_ported = property_leaf_grammar_is_fully_ported(property);
    let mut parsed_values = Vec::new();
    for item in values.split(ComponentValue::is_comma) {
        let mut item_values = item.iter().filter(|value| !value.is_whitespace());
        let Some(value) = item_values.next() else {
            return if fully_ported {
                ParseOutcome::Invalid
            } else {
                ParseOutcome::NotHandled
            };
        };
        if item_values.next().is_some() {
            // NB: Operator expressions without an explicit calc() wrapper require the calculation
            //     parser path.
            return ParseOutcome::NotHandled;
        }
        let Some(parsed) = parse_single_property_leaf(context, property, value) else {
            return if fully_ported {
                ParseOutcome::Invalid
            } else {
                ParseOutcome::NotHandled
            };
        };
        parsed_values.push(parsed);
    }
    ParseOutcome::Parsed(shared_style_value(value_list(parsed_values, 1, true)))
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
        return ParseOutcome::NotHandled;
    }
    if contains_unported_function(values) {
        return ParseOutcome::NotHandled;
    }

    let Some(value) = single_non_whitespace_value(values) else {
        return ParseOutcome::NotHandled;
    };
    if let Some(parsed) = parse_single_property_leaf(context, property, value) {
        return ParseOutcome::Parsed(shared_style_value(parsed));
    }
    if property_leaf_grammar_is_fully_ported(property) {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::NotHandled
}

fn parse_generic_space_separated_value_list(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    let maximum_value_count = property_maximum_value_count(property);
    if property_is_shorthand(property)
        || property_has_coordinating_list_multiplicity(property)
        || property_uses_special_keyword_parser(property)
        || maximum_value_count == 1
    {
        return ParseOutcome::NotHandled;
    }
    if contains_unported_function(values) {
        return ParseOutcome::NotHandled;
    }

    let parsed_values = values
        .iter()
        .filter(|value| !value.is_whitespace())
        .map(|value| parse_single_property_leaf(context, property, value))
        .collect::<Option<Vec<_>>>();
    let Some(parsed_values) = parsed_values else {
        return if property_leaf_grammar_is_fully_ported(property) {
            ParseOutcome::Invalid
        } else {
            ParseOutcome::NotHandled
        };
    };
    if parsed_values.is_empty() || parsed_values.len() > maximum_value_count {
        return ParseOutcome::Invalid;
    }
    match parsed_values.as_slice() {
        [_] => ParseOutcome::Parsed(shared_style_value(parsed_values.into_iter().next().unwrap())),
        _ => ParseOutcome::Parsed(shared_style_value(value_list(parsed_values, 0, true))),
    }
}

fn parse_generic_numeric_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if !(FIRST_SHORTHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property)
        || property_is_shorthand(property)
        || property_has_coordinating_list_multiplicity(property)
        || property_uses_special_keyword_parser(property)
        || !property_uses_numeric_parser(property)
    {
        return ParseOutcome::NotHandled;
    }
    if contains_unported_function(values) && !contains_tree_counting_function(values) {
        return ParseOutcome::NotHandled;
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
        return ParseOutcome::Parsed(shared_style_value(parsed));
    }
    if property_numeric_grammar_is_fully_ported(property) && single_value.is_some() {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::NotHandled
}

fn parse_generic_property_keyword(property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if !(FIRST_SHORTHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property)
        || property_is_shorthand(property)
        || property_has_coordinating_list_multiplicity(property)
        || property_uses_special_keyword_parser(property)
    {
        return ParseOutcome::NotHandled;
    }

    let Some(identifier) = single_non_whitespace_value(values).and_then(ComponentValue::ident) else {
        return ParseOutcome::NotHandled;
    };
    let parsed_keyword = keyword_from_ascii_case_insensitive(identifier);
    if let Some(parsed_keyword) = parsed_keyword
        && property_accepted_keywords(property)
            .binary_search(&parsed_keyword)
            .is_ok()
    {
        let keyword = property_resolve_legacy_value_alias(property, parsed_keyword);
        return ParseOutcome::Parsed(shared_style_value(StyleValueData::Keyword { keyword }));
    }
    if property_accepts_only_keywords(property) {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::NotHandled
}

fn parse_display_keyword(values: &[ComponentValue]) -> ParseOutcome {
    let Some(keywords) = keyword_sequence(values) else {
        return ParseOutcome::Invalid;
    };
    let display = if let [keyword] = keywords.as_slice() {
        let Some(display) = FfiDisplay::from_single_keyword(*keyword) else {
            return ParseOutcome::Invalid;
        };
        display
    } else {
        let mut outside = None;
        let mut inside = None;
        let mut list_item = false;
        for keyword in keywords {
            if keyword == keyword::LIST_ITEM {
                if list_item {
                    return ParseOutcome::Invalid;
                }
                list_item = true;
            } else if let Some(value) = keyword_to_display_inside(keyword) {
                if value == display_inside::_WEBKIT_BOX || inside.replace(value).is_some() {
                    return ParseOutcome::Invalid;
                }
            } else if let Some(value) = keyword_to_display_outside(keyword) {
                if outside.replace(value).is_some() {
                    return ParseOutcome::Invalid;
                }
            } else {
                return ParseOutcome::Invalid;
            }
        }
        if list_item && inside.is_some_and(|inside| !matches!(inside, display_inside::FLOW | display_inside::FLOW_ROOT))
        {
            return ParseOutcome::Invalid;
        }
        FfiDisplay::outside_and_inside(
            outside.unwrap_or(display_outside::BLOCK),
            inside.unwrap_or(display_inside::FLOW),
            list_item,
        )
    };
    ParseOutcome::Parsed(shared_style_value(StyleValueData::Display { raw: display.encoded() }))
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

    if value_contexts.first().is_some_and(|value_context| {
        value_context.kind == FfiValueParsingContextKind::Special && matches!(value_context.value, 0 | 3)
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
        return ParseOutcome::NotHandled;
    }
    let mut stream = TokenStream::new(values);
    let Some(color) = parse_color_value(
        context,
        property,
        &mut stream,
        context_allows_quirky_color(context, property),
    ) else {
        return ParseOutcome::NotHandled;
    };
    stream.discard_whitespace();
    if stream.has_next_token() {
        return ParseOutcome::NotHandled;
    }
    ParseOutcome::Parsed(shared_style_value(color))
}

fn parse_positional_value_list_shorthand(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    if !property_is_positional_value_list_shorthand(property) {
        return ParseOutcome::NotHandled;
    }

    let longhands = longhands_for_shorthand(property);
    let Some(&representative_longhand) = longhands.first() else {
        return ParseOutcome::Invalid;
    };
    let parsed = values
        .iter()
        .filter(|value| !value.is_whitespace())
        .map(
            |value| match parse_css_value(context, representative_longhand, std::slice::from_ref(value)) {
                ParseOutcome::Parsed(value) => Some((*value).clone()),
                ParseOutcome::Invalid | ParseOutcome::NotHandled => None,
            },
        )
        .collect::<Option<Vec<_>>>();
    let Some(parsed) = parsed.filter(|parsed| !parsed.is_empty() && parsed.len() <= longhands.len()) else {
        return ParseOutcome::Invalid;
    };

    let expanded = match longhands.len() {
        2 => match parsed.as_slice() {
            [first] => vec![first.clone(), first.clone()],
            [first, second] => vec![first.clone(), second.clone()],
            _ => return ParseOutcome::Invalid,
        },
        4 => match parsed.as_slice() {
            [first] => vec![first.clone(), first.clone(), first.clone(), first.clone()],
            [first, second] => vec![first.clone(), second.clone(), first.clone(), second.clone()],
            [first, second, third] => vec![first.clone(), second.clone(), third.clone(), second.clone()],
            [first, second, third, fourth] => {
                vec![first.clone(), second.clone(), third.clone(), fourth.clone()]
            }
            _ => return ParseOutcome::Invalid,
        },
        _ => return ParseOutcome::Invalid,
    };
    ParseOutcome::Parsed(shared_style_value(StyleValueData::Shorthand {
        shorthand_property: property,
        sub_properties: RetainedPropertyIdList::from_property_ids(longhands.to_vec()),
        values: RetainedStyleValueDataList::from_retained_values(
            expanded.into_iter().map(RetainedStyleValueData::from_owned).collect(),
        ),
    }))
}

fn parse_initial_longhand(context: &ParseContext, property: u16) -> Option<StyleValueData> {
    let initial = property_initial_value(property);
    let values = consume_a_list_of_component_values(tokenize_for_parser(initial.as_bytes())).ok()?;
    match parse_css_value(context, property, &values) {
        ParseOutcome::Parsed(value) => Some((*value).clone()),
        ParseOutcome::Invalid | ParseOutcome::NotHandled => None,
    }
}

fn shorthand_value(property: u16, sub_properties: Vec<u16>, values: Vec<StyleValueData>) -> StyleValueData {
    StyleValueData::Shorthand {
        shorthand_property: property,
        sub_properties: RetainedPropertyIdList::from_property_ids(sub_properties),
        values: RetainedStyleValueDataList::from_retained_values(
            values.into_iter().map(RetainedStyleValueData::from_owned).collect(),
        ),
    }
}

fn shorthand_with_initial_longhands(context: &ParseContext, property: u16) -> Option<StyleValueData> {
    let longhands = longhands_for_shorthand(property);
    let values = longhands
        .iter()
        .map(|&longhand| parse_initial_longhand(context, longhand))
        .collect::<Option<Vec<_>>>()?;
    Some(shorthand_value(property, longhands.to_vec(), values))
}

fn parse_component_for_property(
    context: &ParseContext,
    property: u16,
    value: &ComponentValue,
) -> Option<StyleValueData> {
    parse_slice_for_property(context, property, std::slice::from_ref(value))
}

fn parse_slice_for_property(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let mut non_whitespace = values.iter().filter(|value| !value.is_whitespace());
    if non_whitespace.next().is_some_and(|value| {
        value
            .ident()
            .and_then(keyword_from_ascii_case_insensitive)
            .is_some_and(is_css_wide_keyword)
    }) && non_whitespace.next().is_none()
    {
        return None;
    }
    match parse_css_value(context, property, values) {
        ParseOutcome::Parsed(value) => {
            if property_has_coordinating_list_multiplicity(property)
                && let StyleValueData::ValueList { values, .. } = &*value
            {
                return values.as_slice().first()?.optional_data().cloned();
            }
            Some((*value).clone())
        }
        ParseOutcome::Invalid | ParseOutcome::NotHandled => None,
    }
}

fn parse_unordered_shorthand(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if !matches!(
        property,
        property_id::BORDER_BLOCK_END
            | property_id::BORDER_BLOCK_START
            | property_id::BORDER_BOTTOM
            | property_id::BORDER_INLINE_END
            | property_id::BORDER_INLINE_START
            | property_id::BORDER_LEFT
            | property_id::BORDER_RIGHT
            | property_id::BORDER_TOP
            | property_id::FLEX_FLOW
            | property_id::OUTLINE
            | property_id::TEXT_WRAP
    ) {
        return ParseOutcome::NotHandled;
    }

    let longhands = longhands_for_shorthand(property);
    let mut parsed = vec![None; longhands.len()];
    let mut parsed_order = Vec::with_capacity(longhands.len());
    let mut parsed_any = false;
    for value in values.iter().filter(|value| !value.is_whitespace()) {
        if value
            .ident()
            .and_then(keyword_from_ascii_case_insensitive)
            .is_some_and(is_css_wide_keyword)
        {
            return ParseOutcome::Invalid;
        }
        let mut matched = false;
        for (index, &longhand) in longhands.iter().enumerate() {
            if parsed[index].is_some() {
                continue;
            }
            if let ParseOutcome::Parsed(value) = parse_css_value(context, longhand, std::slice::from_ref(value)) {
                parsed[index] = Some((*value).clone());
                parsed_order.push(index);
                matched = true;
                parsed_any = true;
                break;
            }
        }
        if !matched {
            return ParseOutcome::Invalid;
        }
    }
    if !parsed_any {
        return ParseOutcome::Invalid;
    }

    let Some(parsed) = parsed
        .into_iter()
        .zip(longhands)
        .map(|(value, &longhand)| value.or_else(|| parse_initial_longhand(context, longhand)))
        .collect::<Option<Vec<_>>>()
    else {
        return ParseOutcome::Invalid;
    };
    let order = if property == property_id::FLEX_FLOW {
        (0..longhands.len()).collect()
    } else {
        for index in 0..longhands.len() {
            if !parsed_order.contains(&index) {
                parsed_order.push(index);
            }
        }
        parsed_order
    };
    ParseOutcome::Parsed(shared_style_value(StyleValueData::Shorthand {
        shorthand_property: property,
        sub_properties: RetainedPropertyIdList::from_property_ids(
            order.iter().map(|&index| longhands[index]).collect(),
        ),
        values: RetainedStyleValueDataList::from_retained_values(
            order
                .into_iter()
                .map(|index| RetainedStyleValueData::from_owned(parsed[index].clone()))
                .collect(),
        ),
    }))
}

fn parse_border_shorthand(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let (width_property, style_property, color_property) = match property {
        property_id::BORDER => (
            property_id::BORDER_WIDTH,
            property_id::BORDER_STYLE,
            property_id::BORDER_COLOR,
        ),
        property_id::BORDER_BLOCK => (
            property_id::BORDER_BLOCK_WIDTH,
            property_id::BORDER_BLOCK_STYLE,
            property_id::BORDER_BLOCK_COLOR,
        ),
        property_id::BORDER_INLINE => (
            property_id::BORDER_INLINE_WIDTH,
            property_id::BORDER_INLINE_STYLE,
            property_id::BORDER_INLINE_COLOR,
        ),
        _ => return ParseOutcome::NotHandled,
    };
    let candidates = [width_property, color_property, style_property];
    let mut parsed = [None, None, None];
    let mut parsed_any = false;
    for value in values.iter().filter(|value| !value.is_whitespace()) {
        let mut matched = false;
        for (index, &candidate) in candidates.iter().enumerate() {
            if parsed[index].is_none()
                && let Some(component) = parse_component_for_property(context, candidate, value)
            {
                parsed[index] = Some(component);
                parsed_any = true;
                matched = true;
                break;
            }
        }
        if !matched {
            return ParseOutcome::Invalid;
        }
    }
    if !parsed_any {
        return ParseOutcome::Invalid;
    }
    let [width, color, style] = parsed;
    let Some(width) = width.or_else(|| shorthand_with_initial_longhands(context, width_property)) else {
        return ParseOutcome::Invalid;
    };
    let Some(style) = style.or_else(|| shorthand_with_initial_longhands(context, style_property)) else {
        return ParseOutcome::Invalid;
    };
    let Some(color) = color.or_else(|| shorthand_with_initial_longhands(context, color_property)) else {
        return ParseOutcome::Invalid;
    };
    let mut sub_properties = vec![width_property, style_property, color_property];
    let mut parsed = vec![width, style, color];
    if property == property_id::BORDER {
        sub_properties.push(property_id::BORDER_IMAGE);
        let Some(border_image) = shorthand_with_initial_longhands(context, property_id::BORDER_IMAGE) else {
            return ParseOutcome::Invalid;
        };
        parsed.push(border_image);
    }
    ParseOutcome::Parsed(shared_style_value(shorthand_value(property, sub_properties, parsed)))
}

fn parse_flex_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let values = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    if values.is_empty()
        || values.iter().any(|value| {
            value
                .ident()
                .and_then(keyword_from_ascii_case_insensitive)
                .is_some_and(is_css_wide_keyword)
        })
    {
        return ParseOutcome::Invalid;
    }
    let number = |value| StyleValueData::Number { value };
    let percentage = |value| StyleValueData::Percentage { value };
    let auto = || StyleValueData::Keyword { keyword: keyword::AUTO };
    let parsed = if let [value] = values.as_slice() {
        if let Some(grow) = parse_component_for_property(context, property_id::FLEX_GROW, value) {
            vec![grow, number(1.0), percentage(0.0)]
        } else if let Some(basis) = parse_component_for_property(context, property_id::FLEX_BASIS, value) {
            vec![number(1.0), number(1.0), basis]
        } else if value
            .ident()
            .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"none"))
        {
            vec![number(0.0), number(0.0), auto()]
        } else {
            return ParseOutcome::Invalid;
        }
    } else {
        let mut index = 0;
        let mut grow = None;
        let mut shrink = None;
        let mut basis = None;
        while index < values.len() {
            if grow.is_none()
                && let Some(value) = parse_component_for_property(context, property_id::FLEX_GROW, values[index])
            {
                grow = Some(value);
                index += 1;
                if index < values.len() {
                    shrink = parse_component_for_property(context, property_id::FLEX_SHRINK, values[index]);
                    if shrink.is_some() {
                        index += 1;
                    }
                }
                continue;
            }
            if basis.is_none()
                && let Some(value) = parse_component_for_property(context, property_id::FLEX_BASIS, values[index])
            {
                basis = Some(value);
                index += 1;
                continue;
            }
            return ParseOutcome::Invalid;
        }
        vec![
            grow.or_else(|| parse_initial_longhand(context, property_id::FLEX_GROW))
                .unwrap_or_else(|| number(0.0)),
            shrink
                .or_else(|| parse_initial_longhand(context, property_id::FLEX_SHRINK))
                .unwrap_or_else(|| number(1.0)),
            basis.unwrap_or_else(|| percentage(0.0)),
        ]
    };
    ParseOutcome::Parsed(shared_style_value(shorthand_value(
        property_id::FLEX,
        longhands_for_shorthand(property_id::FLEX).to_vec(),
        parsed,
    )))
}

fn parse_columns_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let values = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    let slash = values.iter().position(|value| value.is_delim(b'/'));
    let (columns, height) = if let Some(slash) = slash {
        if slash == 0 || slash > 2 || values.len() != slash + 2 {
            return ParseOutcome::Invalid;
        }
        (&values[..slash], Some(values[slash + 1]))
    } else {
        if values.is_empty() || values.len() > 2 {
            return ParseOutcome::Invalid;
        }
        (values.as_slice(), None)
    };
    let mut count = None;
    let mut width = None;
    let mut auto_count = 0;
    for value in columns {
        if value
            .ident()
            .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"auto"))
        {
            auto_count += 1;
        } else {
            if count.is_none()
                && let Some(parsed_count) = parse_component_for_property(context, property_id::COLUMN_COUNT, value)
            {
                count = Some(parsed_count);
                continue;
            }
            if width.is_none()
                && let Some(parsed_width) = parse_component_for_property(context, property_id::COLUMN_WIDTH, value)
            {
                width = Some(parsed_width);
                continue;
            }
            return ParseOutcome::Invalid;
        }
    }
    if auto_count > 2 {
        return ParseOutcome::Invalid;
    }
    let auto = || StyleValueData::Keyword { keyword: keyword::AUTO };
    if auto_count > 0 {
        count.get_or_insert_with(auto);
        width.get_or_insert_with(auto);
    }
    let Some(count) = count.or_else(|| parse_initial_longhand(context, property_id::COLUMN_COUNT)) else {
        return ParseOutcome::Invalid;
    };
    let Some(width) = width.or_else(|| parse_initial_longhand(context, property_id::COLUMN_WIDTH)) else {
        return ParseOutcome::Invalid;
    };
    let height = match height {
        Some(value) => parse_component_for_property(context, property_id::COLUMN_HEIGHT, value),
        None => parse_initial_longhand(context, property_id::COLUMN_HEIGHT),
    };
    let Some(height) = height else {
        return ParseOutcome::Invalid;
    };
    ParseOutcome::Parsed(shared_style_value(shorthand_value(
        property_id::COLUMNS,
        vec![
            property_id::COLUMN_COUNT,
            property_id::COLUMN_WIDTH,
            property_id::COLUMN_HEIGHT,
        ],
        vec![count, width, height],
    )))
}

fn parse_container_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let slash = values.iter().position(|value| value.is_delim(b'/'));
    let (name_values, type_values) = match slash {
        Some(slash) => (&values[..slash], Some(&values[slash + 1..])),
        None => (values, None),
    };
    if name_values.iter().all(ComponentValue::is_whitespace)
        || type_values.is_some_and(|values| values.iter().all(ComponentValue::is_whitespace))
        || values.iter().filter(|value| value.is_delim(b'/')).count() > 1
    {
        return ParseOutcome::Invalid;
    }
    let ParseOutcome::Parsed(name) = parse_css_value(context, property_id::CONTAINER_NAME, name_values) else {
        return ParseOutcome::Invalid;
    };
    let container_type = match type_values {
        Some(values) => match parse_css_value(context, property_id::CONTAINER_TYPE, values) {
            ParseOutcome::Parsed(value) => (*value).clone(),
            ParseOutcome::Invalid | ParseOutcome::NotHandled => return ParseOutcome::Invalid,
        },
        None => match parse_initial_longhand(context, property_id::CONTAINER_TYPE) {
            Some(value) => value,
            None => return ParseOutcome::Invalid,
        },
    };
    ParseOutcome::Parsed(shared_style_value(shorthand_value(
        property_id::CONTAINER,
        longhands_for_shorthand(property_id::CONTAINER).to_vec(),
        vec![(*name).clone(), container_type],
    )))
}

fn parse_place_shorthand(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let (align, justify) = match property {
        property_id::PLACE_CONTENT => (property_id::ALIGN_CONTENT, property_id::JUSTIFY_CONTENT),
        property_id::PLACE_ITEMS => (property_id::ALIGN_ITEMS, property_id::JUSTIFY_ITEMS),
        property_id::PLACE_SELF => (property_id::ALIGN_SELF, property_id::JUSTIFY_SELF),
        _ => return ParseOutcome::NotHandled,
    };
    if let ParseOutcome::Parsed(align_value) = parse_css_value(context, align, values)
        && matches!(&*align_value, StyleValueData::Keyword { .. })
        && matches!(parse_css_value(context, justify, values), ParseOutcome::Parsed(_))
    {
        return ParseOutcome::Parsed(shared_style_value(shorthand_value(
            property,
            vec![align, justify],
            vec![(*align_value).clone(), (*align_value).clone()],
        )));
    }
    let non_whitespace = values
        .iter()
        .filter(|value| !value.is_whitespace())
        .cloned()
        .collect::<Vec<_>>();
    for split in (1..non_whitespace.len()).rev() {
        let (align_values, justify_values) = non_whitespace.split_at(split);
        let (ParseOutcome::Parsed(align_value), ParseOutcome::Parsed(justify_value)) = (
            parse_css_value(context, align, align_values),
            parse_css_value(context, justify, justify_values),
        ) else {
            continue;
        };
        return ParseOutcome::Parsed(shared_style_value(shorthand_value(
            property,
            vec![align, justify],
            vec![(*align_value).clone(), (*justify_value).clone()],
        )));
    }
    ParseOutcome::Invalid
}

fn parse_list_style_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let longhands = [
        property_id::LIST_STYLE_POSITION,
        property_id::LIST_STYLE_IMAGE,
        property_id::LIST_STYLE_TYPE,
    ];
    let mut parsed = [None, None, None];
    let mut none_count = 0;
    for value in values.iter().filter(|value| !value.is_whitespace()) {
        if value
            .ident()
            .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"none"))
        {
            none_count += 1;
            continue;
        }
        let mut matched = false;
        for (index, &longhand) in longhands.iter().enumerate() {
            if parsed[index].is_none()
                && let Some(component) = parse_component_for_property(context, longhand, value)
            {
                parsed[index] = Some(component);
                matched = true;
                break;
            }
        }
        if !matched {
            return ParseOutcome::Invalid;
        }
    }
    if none_count > 2 || (none_count == 2 && (parsed[1].is_some() || parsed[2].is_some())) {
        return ParseOutcome::Invalid;
    }
    if none_count > 0 {
        if parsed[1].is_some() && parsed[2].is_some() {
            return ParseOutcome::Invalid;
        }
        let none = || StyleValueData::Keyword { keyword: keyword::NONE };
        parsed[1].get_or_insert_with(none);
        parsed[2].get_or_insert_with(none);
    }
    let Some(parsed) = parsed
        .into_iter()
        .zip(longhands)
        .map(|(value, longhand)| value.or_else(|| parse_initial_longhand(context, longhand)))
        .collect::<Option<Vec<_>>>()
    else {
        return ParseOutcome::Invalid;
    };
    ParseOutcome::Parsed(shared_style_value(shorthand_value(
        property_id::LIST_STYLE,
        longhands.to_vec(),
        parsed,
    )))
}

fn parse_white_space_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let components = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
    let keyword_value = |keyword| StyleValueData::Keyword { keyword };
    if let [value] = components.as_slice()
        && let Some(keyword) = value.ident().and_then(keyword_from_ascii_case_insensitive)
    {
        let aliases = match keyword {
            keyword::NORMAL => Some((keyword::COLLAPSE, keyword::WRAP)),
            keyword::PRE => Some((keyword::PRESERVE, keyword::NOWRAP)),
            keyword::PRE_WRAP => Some((keyword::PRESERVE, keyword::WRAP)),
            keyword::PRE_LINE => Some((keyword::PRESERVE_BREAKS, keyword::WRAP)),
            _ => None,
        };
        if let Some((collapse, wrap)) = aliases {
            return ParseOutcome::Parsed(shared_style_value(shorthand_value(
                property_id::WHITE_SPACE,
                longhands_for_shorthand(property_id::WHITE_SPACE).to_vec(),
                vec![
                    keyword_value(collapse),
                    keyword_value(wrap),
                    keyword_value(keyword::NONE),
                ],
            )));
        }
    }

    let longhands = [
        property_id::WHITE_SPACE_COLLAPSE,
        property_id::TEXT_WRAP_MODE,
        property_id::WHITE_SPACE_TRIM,
    ];
    let mut parsed = [None, None, None];
    let mut index = 0;
    while index < components.len() {
        if parsed[0].is_none()
            && let Some(value) = parse_component_for_property(context, longhands[0], components[index])
        {
            parsed[0] = Some(value);
            index += 1;
            continue;
        }
        if parsed[1].is_none()
            && let Some(value) = parse_component_for_property(context, longhands[1], components[index])
        {
            parsed[1] = Some(value);
            index += 1;
            continue;
        }
        if parsed[2].is_none() {
            let start = index;
            while index < components.len()
                && components[index]
                    .ident()
                    .and_then(keyword_from_ascii_case_insensitive)
                    .is_some_and(|keyword| property_accepted_keywords(longhands[2]).binary_search(&keyword).is_ok())
            {
                index += 1;
            }
            let trim_values = components[start..index]
                .iter()
                .map(|value| (*value).clone())
                .collect::<Vec<_>>();
            match parse_css_value(context, longhands[2], &trim_values) {
                ParseOutcome::Parsed(value) => {
                    parsed[2] = Some((*value).clone());
                    continue;
                }
                ParseOutcome::Invalid | ParseOutcome::NotHandled => {}
            }
        }
        return ParseOutcome::Invalid;
    }
    if components.is_empty() {
        return ParseOutcome::Invalid;
    }
    let Some(parsed) = parsed
        .into_iter()
        .zip(longhands)
        .map(|(value, longhand)| value.or_else(|| parse_initial_longhand(context, longhand)))
        .collect::<Option<Vec<_>>>()
    else {
        return ParseOutcome::Invalid;
    };
    ParseOutcome::Parsed(shared_style_value(shorthand_value(
        property_id::WHITE_SPACE,
        longhands.to_vec(),
        parsed,
    )))
}

fn parse_overflow_clip_margin_shorthand(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    if !matches!(
        property,
        property_id::OVERFLOW_CLIP_MARGIN
            | property_id::OVERFLOW_CLIP_MARGIN_BLOCK
            | property_id::OVERFLOW_CLIP_MARGIN_INLINE
    ) {
        return ParseOutcome::NotHandled;
    }
    let Some(&longhand) = longhands_for_shorthand(property).first() else {
        return ParseOutcome::Invalid;
    };
    let ParseOutcome::Parsed(value) = parse_css_value(context, longhand, values) else {
        return ParseOutcome::Invalid;
    };
    let longhands = longhands_for_shorthand(property);
    ParseOutcome::Parsed(shared_style_value(shorthand_value(
        property,
        longhands.to_vec(),
        vec![(*value).clone(); longhands.len()],
    )))
}

fn assign_shorthand_components(
    context: &ParseContext,
    components: &[ComponentValue],
    properties: &[u16],
    offset: usize,
    parsed: &mut [Option<StyleValueData>],
) -> bool {
    if offset == components.len() {
        return true;
    }
    for (property_index, &property) in properties.iter().enumerate() {
        if parsed[property_index].is_some() {
            continue;
        }
        for end in (offset + 1..=components.len()).rev() {
            let Some(value) = parse_slice_for_property(context, property, &components[offset..end]) else {
                continue;
            };
            parsed[property_index] = Some(value);
            if assign_shorthand_components(context, components, properties, end, parsed) {
                return true;
            }
            parsed[property_index] = None;
        }
    }
    false
}

fn parse_text_decoration_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let components = values
        .iter()
        .filter(|value| !value.is_whitespace())
        .cloned()
        .collect::<Vec<_>>();
    if components.is_empty() {
        return ParseOutcome::Invalid;
    }
    let parse_order = [
        property_id::TEXT_DECORATION_COLOR,
        property_id::TEXT_DECORATION_LINE,
        property_id::TEXT_DECORATION_STYLE,
        property_id::TEXT_DECORATION_THICKNESS,
    ];
    let mut parsed = [None, None, None, None];
    if !assign_shorthand_components(context, &components, &parse_order, 0, &mut parsed) {
        return ParseOutcome::Invalid;
    }
    let output_order = [
        property_id::TEXT_DECORATION_LINE,
        property_id::TEXT_DECORATION_THICKNESS,
        property_id::TEXT_DECORATION_STYLE,
        property_id::TEXT_DECORATION_COLOR,
    ];
    let Some(output) = output_order
        .iter()
        .map(|&property| {
            let index = parse_order.iter().position(|candidate| *candidate == property)?;
            parsed[index]
                .clone()
                .or_else(|| parse_initial_longhand(context, property))
        })
        .collect::<Option<Vec<_>>>()
    else {
        return ParseOutcome::Invalid;
    };
    ParseOutcome::Parsed(shared_style_value(shorthand_value(
        property_id::TEXT_DECORATION,
        output_order.to_vec(),
        output,
    )))
}

fn assign_border_image_outer_components(
    context: &ParseContext,
    components: &[ComponentValue],
    parsed: &mut [Option<StyleValueData>; 5],
) -> bool {
    let properties = [
        property_id::BORDER_IMAGE_SOURCE,
        property_id::BORDER_IMAGE_REPEAT,
        property_id::BORDER_IMAGE_SLICE,
    ];
    let mut outer = [parsed[0].clone(), parsed[4].clone(), parsed[1].clone()];
    if !assign_shorthand_components(context, components, &properties, 0, &mut outer) {
        return false;
    }
    parsed[0] = outer[0].take();
    parsed[4] = outer[1].take();
    parsed[1] = outer[2].take();
    true
}

fn parse_border_image_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let mut sections = Vec::new();
    let mut current = Vec::new();
    for value in values.iter().filter(|value| !value.is_whitespace()) {
        if value.is_delim(b'/') {
            sections.push(current);
            current = Vec::new();
        } else {
            current.push(value.clone());
        }
    }
    sections.push(current);
    if sections.is_empty() || sections.len() > 3 || sections[0].is_empty() {
        return ParseOutcome::Invalid;
    }

    let mut base = [None, None, None, None, None];
    if !assign_border_image_outer_components(context, &sections[0], &mut base)
        || sections.len() > 1 && base[1].is_none()
    {
        return ParseOutcome::Invalid;
    }
    let width_ends: Vec<usize> = if sections.len() == 1 {
        vec![0]
    } else if sections.len() == 2 {
        (1..=sections[1].len()).rev().collect()
    } else {
        vec![sections[1].len()]
    };
    for width_end in width_ends {
        let mut parsed = base.clone();
        if width_end > 0 {
            let Some(width) =
                parse_slice_for_property(context, property_id::BORDER_IMAGE_WIDTH, &sections[1][..width_end])
            else {
                continue;
            };
            parsed[2] = Some(width);
        }
        if sections.len() > 1 && !assign_border_image_outer_components(context, &sections[1][width_end..], &mut parsed)
        {
            continue;
        }
        let outset_ends: Vec<usize> = if sections.len() == 3 {
            (1..=sections[2].len()).rev().collect()
        } else {
            vec![0]
        };
        for outset_end in outset_ends {
            let mut candidate = parsed.clone();
            if outset_end > 0 {
                let Some(outset) =
                    parse_slice_for_property(context, property_id::BORDER_IMAGE_OUTSET, &sections[2][..outset_end])
                else {
                    continue;
                };
                candidate[3] = Some(outset);
            }
            if sections.len() == 3
                && !assign_border_image_outer_components(context, &sections[2][outset_end..], &mut candidate)
            {
                continue;
            }
            let longhands = longhands_for_shorthand(property_id::BORDER_IMAGE);
            let Some(output) = candidate
                .into_iter()
                .zip(longhands)
                .map(|(value, &longhand)| value.or_else(|| parse_initial_longhand(context, longhand)))
                .collect::<Option<Vec<_>>>()
            else {
                return ParseOutcome::Invalid;
            };
            return ParseOutcome::Parsed(shared_style_value(shorthand_value(
                property_id::BORDER_IMAGE,
                longhands.to_vec(),
                output,
            )));
        }
    }
    ParseOutcome::Invalid
}

fn parse_font_variant_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let components = values
        .iter()
        .filter(|value| !value.is_whitespace())
        .cloned()
        .collect::<Vec<_>>();
    if components.is_empty() {
        return ParseOutcome::Invalid;
    }
    if components.len() > 1
        && components.iter().any(|value| {
            value.ident().is_some_and(|ident| {
                equals_ascii_case_insensitive(ident, b"normal") || equals_ascii_case_insensitive(ident, b"none")
            })
        })
    {
        return ParseOutcome::Invalid;
    }
    let parse_order = [
        property_id::FONT_VARIANT_LIGATURES,
        property_id::FONT_VARIANT_ALTERNATES,
        property_id::FONT_VARIANT_NUMERIC,
        property_id::FONT_VARIANT_EAST_ASIAN,
        property_id::FONT_VARIANT_CAPS,
        property_id::FONT_VARIANT_EMOJI,
        property_id::FONT_VARIANT_POSITION,
    ];
    let mut parsed: [Option<StyleValueData>; 7] = Default::default();
    let single_keyword = components.len() == 1
        && components[0].ident().is_some_and(|ident| {
            equals_ascii_case_insensitive(ident, b"normal") || equals_ascii_case_insensitive(ident, b"none")
        });
    if single_keyword {
        if components[0]
            .ident()
            .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"none"))
        {
            parsed[0] = Some(StyleValueData::Keyword { keyword: keyword::NONE });
        }
    } else if !assign_shorthand_components(context, &components, &parse_order, 0, &mut parsed) {
        return ParseOutcome::Invalid;
    }
    let output_order = longhands_for_shorthand(property_id::FONT_VARIANT);
    let Some(output) = output_order
        .iter()
        .map(|&property| {
            let index = parse_order.iter().position(|candidate| *candidate == property)?;
            parsed[index]
                .clone()
                .or_else(|| parse_initial_longhand(context, property))
        })
        .collect::<Option<Vec<_>>>()
    else {
        return ParseOutcome::Invalid;
    };
    ParseOutcome::Parsed(shared_style_value(shorthand_value(
        property_id::FONT_VARIANT,
        output_order.to_vec(),
        output,
    )))
}

fn parse_font_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let components = values
        .iter()
        .filter(|value| !value.is_whitespace())
        .cloned()
        .collect::<Vec<_>>();
    let mut font_style = None;
    let mut font_variant = None;
    let mut font_weight = None;
    let mut font_width = None;
    let mut normal_count = 0;
    let mut index = 0;
    while index < components.len() {
        let component = &components[index];
        if component
            .ident()
            .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"normal"))
        {
            normal_count += 1;
            index += 1;
            continue;
        }
        if font_variant.is_none()
            && component
                .ident()
                .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"small-caps"))
        {
            let ParseOutcome::Parsed(value) = parse_font_variant_shorthand(context, std::slice::from_ref(component))
            else {
                return ParseOutcome::Invalid;
            };
            font_variant = Some((*value).clone());
            index += 1;
            continue;
        }
        if font_width.is_none()
            && let Some(value) = parse_component_for_property(context, property_id::FONT_WIDTH, component)
            && matches!(value, StyleValueData::Keyword { .. })
        {
            font_width = Some(value);
            index += 1;
            continue;
        }
        if let Some(font_size) = parse_component_for_property(context, property_id::FONT_SIZE, component) {
            index += 1;
            let line_height = if components.get(index).is_some_and(|value| value.is_delim(b'/')) {
                index += 1;
                let Some(component) = components.get(index) else {
                    return ParseOutcome::Invalid;
                };
                let Some(value) = parse_component_for_property(context, property_id::LINE_HEIGHT, component) else {
                    return ParseOutcome::Invalid;
                };
                index += 1;
                Some(value)
            } else {
                None
            };
            if index >= components.len() {
                return ParseOutcome::Invalid;
            }
            let Some(font_family) = parse_slice_for_property(context, property_id::FONT_FAMILY, &components[index..])
            else {
                return ParseOutcome::Invalid;
            };
            let unset_count = usize::from(font_style.is_none())
                + usize::from(font_weight.is_none())
                + usize::from(font_variant.is_none())
                + usize::from(font_width.is_none());
            if normal_count > unset_count {
                return ParseOutcome::Invalid;
            }
            let font_variant = match font_variant {
                Some(value) => Some(value),
                None => {
                    let values = consume_a_list_of_component_values(tokenize_for_parser(b"normal")).ok();
                    match values
                        .as_deref()
                        .map(|values| parse_font_variant_shorthand(context, values))
                    {
                        Some(ParseOutcome::Parsed(value)) => Some((*value).clone()),
                        _ => return ParseOutcome::Invalid,
                    }
                }
            };
            let longhands = longhands_for_shorthand(property_id::FONT);
            let explicit = [
                Some(font_family),
                Some(font_size),
                font_width,
                font_style,
                font_variant,
                font_weight,
                line_height,
            ];
            let Some(output) = longhands
                .iter()
                .enumerate()
                .map(|(index, &longhand)| {
                    explicit
                        .get(index)
                        .cloned()
                        .flatten()
                        .or_else(|| parse_initial_longhand(context, longhand))
                })
                .collect::<Option<Vec<_>>>()
            else {
                return ParseOutcome::Invalid;
            };
            return ParseOutcome::Parsed(shared_style_value(shorthand_value(
                property_id::FONT,
                longhands.to_vec(),
                output,
            )));
        }
        if font_style.is_none() {
            let mut parsed_style = None;
            for end in (index + 1..=components.len()).rev() {
                if let Some(value) = parse_slice_for_property(context, property_id::FONT_STYLE, &components[index..end])
                {
                    parsed_style = Some((value, end));
                    break;
                }
            }
            if let Some((value, end)) = parsed_style {
                font_style = Some(value);
                index = end;
                continue;
            }
        }
        if font_weight.is_none()
            && let Some(value) = parse_component_for_property(context, property_id::FONT_WEIGHT, component)
        {
            font_weight = Some(value);
            index += 1;
            continue;
        }
        return ParseOutcome::Invalid;
    }
    ParseOutcome::Invalid
}

fn comma_separated_layers(values: &[ComponentValue]) -> Option<Vec<Vec<ComponentValue>>> {
    if values
        .iter()
        .rev()
        .find(|value| !value.is_whitespace())
        .is_some_and(|value| value.is_comma())
    {
        return None;
    }
    let mut layers = Vec::new();
    let mut layer = Vec::new();
    for value in values {
        if value.is_comma() {
            if !layer.iter().any(|value: &ComponentValue| !value.is_whitespace()) {
                return None;
            }
            layers.push(layer);
            layer = Vec::new();
        } else if !value.is_whitespace() {
            layer.push(value.clone());
        }
    }
    if !layer.is_empty() {
        layers.push(layer);
    }
    (!layers.is_empty()).then_some(layers)
}

fn single_coordinating_value(value: StyleValueData) -> Option<StyleValueData> {
    match value {
        StyleValueData::ValueList { values, .. } => values.as_slice().first()?.optional_data().cloned(),
        value => Some(value),
    }
}

fn parse_layer_with_optional_size<const N: usize>(
    context: &ParseContext,
    layer: &[ComponentValue],
    properties: &[u16; N],
    position_index: usize,
    size_property: u16,
) -> Option<([Option<StyleValueData>; N], Option<StyleValueData>)> {
    let slash = layer.iter().position(|value| value.is_delim(b'/'));
    if layer.iter().filter(|value| value.is_delim(b'/')).count() > 1 {
        return None;
    }
    let before_slash = slash.map_or(layer, |index| &layer[..index]);
    let mut parsed: [Option<StyleValueData>; N] = std::array::from_fn(|_| None);
    if !assign_shorthand_components(context, before_slash, properties, 0, &mut parsed) {
        return None;
    }
    let Some(slash) = slash else {
        return Some((parsed, None));
    };
    if parsed[position_index].is_none() || slash + 1 == layer.len() {
        return None;
    }
    let after_slash = &layer[slash + 1..];
    for end in (1..=after_slash.len()).rev() {
        let Some(size) = parse_slice_for_property(context, size_property, &after_slash[..end]) else {
            continue;
        };
        let mut candidate = parsed.clone();
        if assign_shorthand_components(context, &after_slash[end..], properties, 0, &mut candidate) {
            return Some((candidate, Some(size)));
        }
    }
    None
}

fn parse_background_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let Some(layers) = comma_separated_layers(values) else {
        return ParseOutcome::Invalid;
    };
    let properties = [
        property_id::BACKGROUND_ATTACHMENT,
        property_id::BACKGROUND_CLIP,
        property_id::BACKGROUND_COLOR,
        property_id::BACKGROUND_IMAGE,
        property_id::BACKGROUND_ORIGIN,
        property_id::BACKGROUND_POSITION,
        property_id::BACKGROUND_REPEAT,
    ];
    let mut images = Vec::new();
    let mut position_xs = Vec::new();
    let mut position_ys = Vec::new();
    let mut sizes = Vec::new();
    let mut repeats = Vec::new();
    let mut attachments = Vec::new();
    let mut origins = Vec::new();
    let mut clips = Vec::new();
    let mut color = None;
    for (layer_index, layer) in layers.iter().enumerate() {
        let Some((mut parsed, size)) =
            parse_layer_with_optional_size(context, layer, &properties, 5, property_id::BACKGROUND_SIZE)
        else {
            return ParseOutcome::Invalid;
        };
        if parsed.iter().all(Option::is_none) && size.is_none()
            || parsed[2].is_some() && layer_index + 1 != layers.len()
        {
            return ParseOutcome::Invalid;
        }
        if let Some(layer_color) = parsed[2].take()
            && color.replace(layer_color).is_some()
        {
            return ParseOutcome::Invalid;
        }
        let (position_x, position_y) = if let Some(position) = parsed[5].take() {
            let StyleValueData::Shorthand { values, .. } = position else {
                return ParseOutcome::Invalid;
            };
            let [x, y] = values.as_slice() else {
                return ParseOutcome::Invalid;
            };
            let Some(x) = x.optional_data().cloned().and_then(single_coordinating_value) else {
                return ParseOutcome::Invalid;
            };
            let Some(y) = y.optional_data().cloned().and_then(single_coordinating_value) else {
                return ParseOutcome::Invalid;
            };
            (x, y)
        } else {
            let Some(x) = coordinating_initial_item(context, property_id::BACKGROUND_POSITION_X) else {
                return ParseOutcome::Invalid;
            };
            let Some(y) = coordinating_initial_item(context, property_id::BACKGROUND_POSITION_Y) else {
                return ParseOutcome::Invalid;
            };
            (x, y)
        };
        let Some(image) = parsed[3]
            .take()
            .or_else(|| coordinating_initial_item(context, property_id::BACKGROUND_IMAGE))
        else {
            return ParseOutcome::Invalid;
        };
        let Some(size) = size.or_else(|| coordinating_initial_item(context, property_id::BACKGROUND_SIZE)) else {
            return ParseOutcome::Invalid;
        };
        let Some(repeat) = parsed[6]
            .take()
            .or_else(|| coordinating_initial_item(context, property_id::BACKGROUND_REPEAT))
        else {
            return ParseOutcome::Invalid;
        };
        let Some(attachment) = parsed[0]
            .take()
            .or_else(|| coordinating_initial_item(context, property_id::BACKGROUND_ATTACHMENT))
        else {
            return ParseOutcome::Invalid;
        };
        let (origin, clip) = match (parsed[1].take(), parsed[4].take()) {
            (Some(first), Some(second)) => (first, second),
            (Some(first), None) => (first.clone(), first),
            (None, Some(first)) => (first.clone(), first),
            (None, None) => {
                let Some(origin) = coordinating_initial_item(context, property_id::BACKGROUND_ORIGIN) else {
                    return ParseOutcome::Invalid;
                };
                let Some(clip) = coordinating_initial_item(context, property_id::BACKGROUND_CLIP) else {
                    return ParseOutcome::Invalid;
                };
                (origin, clip)
            }
        };
        images.push(image);
        position_xs.push(position_x);
        position_ys.push(position_y);
        sizes.push(size);
        repeats.push(repeat);
        attachments.push(attachment);
        origins.push(origin);
        clips.push(clip);
    }
    let Some(color) = color.or_else(|| parse_initial_longhand(context, property_id::BACKGROUND_COLOR)) else {
        return ParseOutcome::Invalid;
    };
    let position = shorthand_value(
        property_id::BACKGROUND_POSITION,
        vec![property_id::BACKGROUND_POSITION_X, property_id::BACKGROUND_POSITION_Y],
        vec![value_list(position_xs, 1, true), value_list(position_ys, 1, true)],
    );
    let output_order = [
        property_id::BACKGROUND_COLOR,
        property_id::BACKGROUND_IMAGE,
        property_id::BACKGROUND_POSITION,
        property_id::BACKGROUND_SIZE,
        property_id::BACKGROUND_REPEAT,
        property_id::BACKGROUND_ATTACHMENT,
        property_id::BACKGROUND_ORIGIN,
        property_id::BACKGROUND_CLIP,
    ];
    ParseOutcome::Parsed(shared_style_value(shorthand_value(
        property_id::BACKGROUND,
        output_order.to_vec(),
        vec![
            color,
            value_list(images, 1, true),
            position,
            value_list(sizes, 1, true),
            value_list(repeats, 1, true),
            value_list(attachments, 1, true),
            value_list(origins, 1, true),
            value_list(clips, 1, true),
        ],
    )))
}

fn parse_mask_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let Some(layers) = comma_separated_layers(values) else {
        return ParseOutcome::Invalid;
    };
    let properties = [
        property_id::MASK_IMAGE,
        property_id::MASK_POSITION,
        property_id::MASK_REPEAT,
        property_id::MASK_ORIGIN,
        property_id::MASK_CLIP,
        property_id::MASK_COMPOSITE,
        property_id::MASK_MODE,
    ];
    let mut coordinated: [Vec<StyleValueData>; 8] = std::array::from_fn(|_| Vec::new());
    for layer in &layers {
        let Some((mut parsed, size)) =
            parse_layer_with_optional_size(context, layer, &properties, 1, property_id::MASK_SIZE)
        else {
            return ParseOutcome::Invalid;
        };
        if parsed.iter().all(Option::is_none) && size.is_none() {
            return ParseOutcome::Invalid;
        }
        let (origin, clip) = match (parsed[3].take(), parsed[4].take()) {
            (Some(origin), Some(clip)) => (origin, clip),
            (Some(origin), None) => (origin.clone(), origin),
            (None, Some(clip)) => {
                let Some(origin) = coordinating_initial_item(context, property_id::MASK_ORIGIN) else {
                    return ParseOutcome::Invalid;
                };
                (origin, clip)
            }
            (None, None) => {
                let Some(origin) = coordinating_initial_item(context, property_id::MASK_ORIGIN) else {
                    return ParseOutcome::Invalid;
                };
                (origin.clone(), origin)
            }
        };
        let values = [
            parsed[0]
                .take()
                .or_else(|| coordinating_initial_item(context, property_id::MASK_IMAGE)),
            parsed[1]
                .take()
                .or_else(|| coordinating_initial_item(context, property_id::MASK_POSITION)),
            size.or_else(|| coordinating_initial_item(context, property_id::MASK_SIZE)),
            parsed[2]
                .take()
                .or_else(|| coordinating_initial_item(context, property_id::MASK_REPEAT)),
            Some(origin),
            Some(clip),
            parsed[5]
                .take()
                .or_else(|| coordinating_initial_item(context, property_id::MASK_COMPOSITE)),
            parsed[6]
                .take()
                .or_else(|| coordinating_initial_item(context, property_id::MASK_MODE)),
        ];
        for (index, value) in values.into_iter().enumerate() {
            let Some(value) = value else {
                return ParseOutcome::Invalid;
            };
            coordinated[index].push(value);
        }
    }
    let output_order = [
        property_id::MASK_IMAGE,
        property_id::MASK_POSITION,
        property_id::MASK_SIZE,
        property_id::MASK_REPEAT,
        property_id::MASK_ORIGIN,
        property_id::MASK_CLIP,
        property_id::MASK_COMPOSITE,
        property_id::MASK_MODE,
    ];
    let multiple = layers.len() > 1;
    let output = coordinated
        .into_iter()
        .map(|mut values| {
            if multiple {
                value_list(values, 1, true)
            } else {
                values.remove(0)
            }
        })
        .collect();
    ParseOutcome::Parsed(shared_style_value(shorthand_value(
        property_id::MASK,
        output_order.to_vec(),
        output,
    )))
}

fn coordinating_initial_item(context: &ParseContext, property: u16) -> Option<StyleValueData> {
    let initial = parse_initial_longhand(context, property)?;
    match initial {
        StyleValueData::ValueList { values, .. } => values.as_slice().first()?.optional_data().cloned(),
        value => Some(value),
    }
}

fn parse_timeline_shorthand(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let (name_property, axis_property, inset_property) = match property {
        property_id::SCROLL_TIMELINE => (
            property_id::SCROLL_TIMELINE_NAME,
            property_id::SCROLL_TIMELINE_AXIS,
            None,
        ),
        property_id::VIEW_TIMELINE => (
            property_id::VIEW_TIMELINE_NAME,
            property_id::VIEW_TIMELINE_AXIS,
            Some(property_id::VIEW_TIMELINE_INSET),
        ),
        _ => return ParseOutcome::NotHandled,
    };
    let mut names = Vec::new();
    let mut axes = Vec::new();
    let mut insets = Vec::new();
    for layer in values.split(ComponentValue::is_comma) {
        let components = layer
            .iter()
            .filter(|value| !value.is_whitespace())
            .cloned()
            .collect::<Vec<_>>();
        let Some(name) = components
            .first()
            .and_then(|value| parse_component_for_property(context, name_property, value))
        else {
            return ParseOutcome::Invalid;
        };
        names.push(name);
        if let Some(inset_property) = inset_property {
            let properties = [axis_property, inset_property];
            let mut parsed = [None, None];
            if !assign_shorthand_components(context, &components[1..], &properties, 0, &mut parsed) {
                return ParseOutcome::Invalid;
            }
            let Some(axis) = parsed[0]
                .take()
                .or_else(|| coordinating_initial_item(context, axis_property))
            else {
                return ParseOutcome::Invalid;
            };
            let Some(inset) = parsed[1]
                .take()
                .or_else(|| coordinating_initial_item(context, inset_property))
            else {
                return ParseOutcome::Invalid;
            };
            axes.push(axis);
            insets.push(inset);
        } else {
            if components.len() > 2 {
                return ParseOutcome::Invalid;
            }
            let axis = if let Some(value) = components.get(1) {
                parse_component_for_property(context, axis_property, value)
            } else {
                coordinating_initial_item(context, axis_property)
            };
            let Some(axis) = axis else {
                return ParseOutcome::Invalid;
            };
            axes.push(axis);
        }
    }
    if names.is_empty() {
        return ParseOutcome::Invalid;
    }
    let mut sub_properties = vec![name_property, axis_property];
    let mut output = vec![value_list(names, 1, true), value_list(axes, 1, true)];
    if let Some(inset_property) = inset_property {
        sub_properties.push(inset_property);
        output.push(value_list(insets, 1, true));
    }
    ParseOutcome::Parsed(shared_style_value(shorthand_value(property, sub_properties, output)))
}

fn parse_animation_transition_shorthand(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    let (parsed_longhands, reset_only) = match property {
        property_id::ANIMATION => (
            &[
                property_id::ANIMATION_DURATION,
                property_id::ANIMATION_TIMING_FUNCTION,
                property_id::ANIMATION_DELAY,
                property_id::ANIMATION_ITERATION_COUNT,
                property_id::ANIMATION_DIRECTION,
                property_id::ANIMATION_FILL_MODE,
                property_id::ANIMATION_PLAY_STATE,
                property_id::ANIMATION_NAME,
            ][..],
            Some(property_id::ANIMATION_TIMELINE),
        ),
        property_id::TRANSITION => (
            &[
                property_id::TRANSITION_PROPERTY,
                property_id::TRANSITION_DURATION,
                property_id::TRANSITION_TIMING_FUNCTION,
                property_id::TRANSITION_DELAY,
                property_id::TRANSITION_BEHAVIOR,
            ][..],
            None,
        ),
        _ => return ParseOutcome::NotHandled,
    };
    let layers = values.split(ComponentValue::is_comma).collect::<Vec<_>>();
    let mut coordinated = vec![Vec::with_capacity(layers.len()); parsed_longhands.len()];
    for layer in &layers {
        let components = layer
            .iter()
            .filter(|value| !value.is_whitespace())
            .cloned()
            .collect::<Vec<_>>();
        if components.is_empty() {
            return ParseOutcome::Invalid;
        }
        let mut parsed = vec![None; parsed_longhands.len()];
        if !assign_shorthand_components(context, &components, parsed_longhands, 0, &mut parsed) {
            return ParseOutcome::Invalid;
        }
        for (index, &longhand) in parsed_longhands.iter().enumerate() {
            let Some(value) = parsed[index]
                .take()
                .or_else(|| coordinating_initial_item(context, longhand))
            else {
                return ParseOutcome::Invalid;
            };
            coordinated[index].push(value);
        }
    }
    if property == property_id::TRANSITION
        && layers.len() > 1
        && coordinated[0]
            .iter()
            .any(|value| matches!(value, StyleValueData::Keyword { keyword: keyword::NONE }))
    {
        return ParseOutcome::Invalid;
    }
    let mut sub_properties = parsed_longhands.to_vec();
    let mut output = coordinated
        .into_iter()
        .map(|values| value_list(values, 1, true))
        .collect::<Vec<_>>();
    if let Some(reset_only) = reset_only {
        sub_properties.push(reset_only);
        let Some(initial) = parse_initial_longhand(context, reset_only) else {
            return ParseOutcome::Invalid;
        };
        output.push(initial);
    }
    ParseOutcome::Parsed(shared_style_value(shorthand_value(property, sub_properties, output)))
}

fn trim_ascii_whitespace(code_units: &[u16]) -> &[u16] {
    let is_ascii_whitespace = |unit: &u16| matches!(*unit, 0x09 | 0x0a | 0x0c | 0x0d | 0x20);
    let start = code_units
        .iter()
        .position(|unit| !is_ascii_whitespace(unit))
        .unwrap_or(code_units.len());
    let end = code_units
        .iter()
        .rposition(|unit| !is_ascii_whitespace(unit))
        .map_or(start, |position| position + 1);
    &code_units[start..end]
}

pub(crate) fn unresolved_value(
    unresolved_source: &[u16],
    comparison_source: &[u16],
    presence: SubstitutionFunctionsPresence,
) -> StyleValueData {
    let source_text = trim_ascii_whitespace(unresolved_source);
    let component_source = if comparison_source.is_empty() {
        source_text
    } else {
        comparison_source
    };
    StyleValueData::Unresolved {
        components: crate::css::style_value::RetainedComponentValueList::from_source(component_source),
        source_text: RetainedReadableString::from_utf16(source_text),
        value_comparison_text: RetainedReadableString::from_utf16(comparison_source),
        presence_attr: presence.attr,
        presence_dashed_function: presence.dashed_function,
        presence_env: presence.env,
        presence_if: presence.if_,
        presence_inherit: presence.inherit,
        presence_var: presence.var,
        contains_attr_tainted_values: false,
        parsed_value: RetainedStyleValueData::none(),
    }
}

pub(crate) fn parse_css_value_with_source(
    context: &ParseContext,
    property_id: u16,
    values: &[ComponentValue],
    unresolved_source: &[u16],
    comparison_source: &[u16],
) -> ParseOutcome {
    if values
        .iter()
        .any(|value| matches!(value.kind, ComponentKind::Token(ParserTokenKind::Semicolon)))
    {
        return ParseOutcome::Invalid;
    }

    let mut substitution_presence = SubstitutionFunctionsPresence::default();
    if collect_arbitrary_substitution_function_presence(values, &mut substitution_presence).is_err() {
        return ParseOutcome::Invalid;
    }
    parse_css_value_after_substitution_scan(
        context,
        property_id,
        values,
        unresolved_source,
        comparison_source,
        substitution_presence,
    )
}

fn parse_css_value_after_substitution_scan(
    context: &ParseContext,
    property_id: u16,
    values: &[ComponentValue],
    unresolved_source: &[u16],
    comparison_source: &[u16],
    substitution_presence: SubstitutionFunctionsPresence,
) -> ParseOutcome {
    if let Some(value) = parse_builtin_value(values) {
        return ParseOutcome::Parsed(shared_style_value(value));
    }
    if property_id == property_id::CUSTOM || substitution_presence.has_any() {
        let declaration_values = values
            .iter()
            .position(|value| !value.is_whitespace())
            .map_or(&values[values.len()..], |position| &values[position..]);
        if !declaration_values.is_empty() && !declaration_value_is_valid(declaration_values) {
            return ParseOutcome::Invalid;
        }
        let mut unresolved = unresolved_value(unresolved_source, comparison_source, substitution_presence);
        if let StyleValueData::Unresolved {
            source_text,
            contains_attr_tainted_values,
            ..
        } = &mut unresolved
        {
            *contains_attr_tainted_values = context.contains_attr_tainted_values;
            if property_id == property_id::CUSTOM && context.is_substituted_value {
                let first_non_whitespace = unresolved_source
                    .iter()
                    .position(|unit| !matches!(*unit, 0x09 | 0x0a | 0x0c | 0x0d | 0x20))
                    .unwrap_or(unresolved_source.len());
                *source_text = RetainedReadableString::from_utf16(&unresolved_source[first_non_whitespace..]);
            }
        }
        return ParseOutcome::Parsed(shared_style_value(unresolved));
    }
    let wrapped_values;
    let values = if let Some(function_names) = substitution_function_context_names(context) {
        let mut nested_values = values.to_vec();
        for function_name in function_names.into_iter().rev() {
            nested_values = vec![ComponentValue {
                kind: ComponentKind::Function {
                    name: function_name.into_boxed_slice().into(),
                    values: nested_values.into_boxed_slice(),
                },
                original_source_text: crate::css::css_tokenizer::ParserSource::empty(),
                opening_source_length: 0,
                closing_source_length: 0,
                start_position: Default::default(),
                end_position: Default::default(),
            }];
        }
        wrapped_values = nested_values;
        &wrapped_values
    } else {
        values
    };
    if property_is_shorthand(property_id) {
        let outcome = parse_grid_property(context, property_id, values);
        if !matches!(outcome, ParseOutcome::NotHandled) {
            return outcome;
        }
        let outcome = parse_geometry_property(context, property_id, values);
        if !matches!(outcome, ParseOutcome::NotHandled) {
            return outcome;
        }
        let outcome = parse_anchor_fit_property(context, property_id, values);
        if !matches!(outcome, ParseOutcome::NotHandled) {
            return outcome;
        }
        let outcome = parse_position_property(context, property_id, values);
        if !matches!(outcome, ParseOutcome::NotHandled) {
            return outcome;
        }
        let outcome = parse_positional_value_list_shorthand(context, property_id, values);
        if !matches!(outcome, ParseOutcome::NotHandled) {
            return outcome;
        }
        let outcome = parse_unordered_shorthand(context, property_id, values);
        if !matches!(outcome, ParseOutcome::NotHandled) {
            return outcome;
        }
        let outcome = parse_border_shorthand(context, property_id, values);
        if !matches!(outcome, ParseOutcome::NotHandled) {
            return outcome;
        }
        if property_id == property_id::FLEX {
            return parse_flex_shorthand(context, values);
        }
        if property_id == property_id::COLUMNS {
            return parse_columns_shorthand(context, values);
        }
        if property_id == property_id::CONTAINER {
            return parse_container_shorthand(context, values);
        }
        let outcome = parse_place_shorthand(context, property_id, values);
        if !matches!(outcome, ParseOutcome::NotHandled) {
            return outcome;
        }
        if property_id == property_id::LIST_STYLE {
            return parse_list_style_shorthand(context, values);
        }
        if property_id == property_id::WHITE_SPACE {
            return parse_white_space_shorthand(context, values);
        }
        let outcome = parse_overflow_clip_margin_shorthand(context, property_id, values);
        if !matches!(outcome, ParseOutcome::NotHandled) {
            return outcome;
        }
        if property_id == property_id::TEXT_DECORATION {
            return parse_text_decoration_shorthand(context, values);
        }
        if property_id == property_id::BORDER_IMAGE {
            return parse_border_image_shorthand(context, values);
        }
        if property_id == property_id::FONT_VARIANT {
            return parse_font_variant_shorthand(context, values);
        }
        if property_id == property_id::FONT {
            return parse_font_shorthand(context, values);
        }
        if property_id == property_id::BACKGROUND {
            return parse_background_shorthand(context, values);
        }
        if property_id == property_id::MASK {
            return parse_mask_shorthand(context, values);
        }
        let outcome = parse_timeline_shorthand(context, property_id, values);
        if !matches!(outcome, ParseOutcome::NotHandled) {
            return outcome;
        }
        let outcome = parse_animation_transition_shorthand(context, property_id, values);
        if !matches!(outcome, ParseOutcome::NotHandled) {
            return outcome;
        }
        if property_id == property_id::ALL {
            return ParseOutcome::Invalid;
        }
        return ParseOutcome::NotHandled;
    }
    let font_outcome = parse_font_property(context, property_id, values);
    if !matches!(font_outcome, ParseOutcome::NotHandled) {
        return font_outcome;
    }
    let grid_outcome = parse_grid_property(context, property_id, values);
    if !matches!(grid_outcome, ParseOutcome::NotHandled) {
        return grid_outcome;
    }
    let long_tail_outcome = parse_long_tail_property(context, property_id, values);
    if !matches!(long_tail_outcome, ParseOutcome::NotHandled) {
        return long_tail_outcome;
    }
    if matches!(
        property_id,
        property_id::CONTAIN
            | property_id::CONTAINER_TYPE
            | property_id::POSITION_VISIBILITY
            | property_id::TEXT_DECORATION_LINE
            | property_id::TOUCH_ACTION
            | property_id::WHITE_SPACE_TRIM
    ) {
        return parse_keyword_combination_property(property_id, values);
    }
    if property_id == property_id::PAINT_ORDER {
        return parse_paint_order_property(values);
    }
    if property_id == property_id::SCROLLBAR_GUTTER {
        return parse_scrollbar_gutter_property(values);
    }
    if property_id == property_id::ASPECT_RATIO {
        return parse_aspect_ratio_property(context, property_id, values);
    }
    if property_id == property_id::MATH_DEPTH {
        return parse_math_depth_property(context, property_id, values);
    }
    if property_id == property_id::SCROLLBAR_COLOR {
        return parse_scrollbar_color_property(context, property_id, values);
    }
    if property_id == property_id::STROKE_DASHARRAY {
        return parse_stroke_dasharray_property(context, property_id, values);
    }
    if property_id == property_id::TEXT_INDENT {
        return parse_text_indent_property(context, property_id, values);
    }
    if property_id == property_id::TEXT_UNDERLINE_POSITION {
        return parse_text_underline_position_property(values);
    }
    if matches!(
        property_id,
        property_id::OVERFLOW_CLIP_MARGIN_BLOCK_END
            | property_id::OVERFLOW_CLIP_MARGIN_BLOCK_START
            | property_id::OVERFLOW_CLIP_MARGIN_BOTTOM
            | property_id::OVERFLOW_CLIP_MARGIN_INLINE_END
            | property_id::OVERFLOW_CLIP_MARGIN_INLINE_START
            | property_id::OVERFLOW_CLIP_MARGIN_LEFT
            | property_id::OVERFLOW_CLIP_MARGIN_RIGHT
            | property_id::OVERFLOW_CLIP_MARGIN_TOP
    ) {
        return parse_overflow_clip_margin_property(context, property_id, values);
    }
    if property_id == property_id::GRID_AUTO_FLOW {
        return parse_grid_auto_flow_property(values);
    }
    if matches!(
        property_id,
        property_id::ALIGN_ITEMS | property_id::ALIGN_SELF | property_id::JUSTIFY_ITEMS | property_id::JUSTIFY_SELF
    ) {
        return parse_alignment_property(property_id, values);
    }
    if matches!(property_id, property_id::FILL | property_id::STROKE) {
        return parse_paint_property(context, property_id, values);
    }
    if property_id == property_id::POSITION {
        let Some(keyword) = single_non_whitespace_value(values)
            .and_then(ComponentValue::ident)
            .and_then(keyword_from_ascii_case_insensitive)
        else {
            return ParseOutcome::Invalid;
        };
        return if property_accepted_keywords(property_id).binary_search(&keyword).is_ok() {
            ParseOutcome::Parsed(shared_style_value(StyleValueData::Keyword { keyword }))
        } else {
            ParseOutcome::Invalid
        };
    }
    if matches!(property_id, property_id::BACKGROUND_REPEAT | property_id::MASK_REPEAT) {
        return parse_repeat_property(values);
    }
    if matches!(property_id, property_id::BACKGROUND_SIZE | property_id::MASK_SIZE) {
        return parse_background_size_property(context, property_id, values);
    }
    if property_id == property_id::BORDER_IMAGE_SLICE {
        return parse_border_image_slice_property(context, property_id, values);
    }
    if matches!(property_id, property_id::BOX_SHADOW | property_id::TEXT_SHADOW) {
        return parse_shadow_property(context, property_id, values);
    }
    if property_id == property_id::POSITION_AREA {
        return parse_position_area_property(values);
    }
    if property_id == property_id::POSITION_TRY_FALLBACKS {
        return parse_position_try_fallbacks_property(context, values);
    }
    if property_id == property_id::TRANSFORM_ORIGIN {
        return parse_transform_origin_property(context, property_id, values);
    }
    if contains_tree_counting_function(values) {
        let tree_counting_outcome = parse_generic_numeric_property(context, property_id, values);
        if !matches!(tree_counting_outcome, ParseOutcome::NotHandled) {
            return tree_counting_outcome;
        }
    }
    if !(FIRST_SHORTHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property_id) {
        return ParseOutcome::NotHandled;
    }
    let transform_effect_outcome = parse_transform_effect_property(context, property_id, values);
    if !matches!(transform_effect_outcome, ParseOutcome::NotHandled) {
        return transform_effect_outcome;
    }
    let image_outcome = parse_image_property(context, property_id, values);
    if !matches!(image_outcome, ParseOutcome::NotHandled) {
        return image_outcome;
    }
    let position_outcome = parse_position_property(context, property_id, values);
    if !matches!(position_outcome, ParseOutcome::NotHandled) {
        return position_outcome;
    }
    let geometry_outcome = parse_geometry_property(context, property_id, values);
    if !matches!(geometry_outcome, ParseOutcome::NotHandled) {
        return geometry_outcome;
    }
    let anchor_fit_outcome = parse_anchor_fit_property(context, property_id, values);
    if !matches!(anchor_fit_outcome, ParseOutcome::NotHandled) {
        return anchor_fit_outcome;
    }
    if property_id == property_id::DISPLAY {
        return parse_display_keyword(values);
    }
    let color_outcome = parse_color_property(context, property_id, values);
    if !matches!(color_outcome, ParseOutcome::NotHandled) {
        return color_outcome;
    }
    let special_text_outcome = parse_special_text_property(context, property_id, values);
    if !matches!(special_text_outcome, ParseOutcome::NotHandled) {
        return special_text_outcome;
    }
    let coordinating_list_outcome = parse_coordinating_value_list(context, property_id, values);
    if !matches!(coordinating_list_outcome, ParseOutcome::NotHandled) {
        return coordinating_list_outcome;
    }
    let space_separated_list_outcome = parse_generic_space_separated_value_list(context, property_id, values);
    if !matches!(space_separated_list_outcome, ParseOutcome::NotHandled) {
        return space_separated_list_outcome;
    }
    let keyword_outcome = parse_generic_property_keyword(property_id, values);
    if !matches!(keyword_outcome, ParseOutcome::NotHandled) {
        return keyword_outcome;
    }
    let text_outcome = parse_generic_text_property(context, property_id, values);
    if !matches!(text_outcome, ParseOutcome::NotHandled) {
        return text_outcome;
    }
    match parse_generic_numeric_property(context, property_id, values) {
        ParseOutcome::NotHandled => ParseOutcome::Invalid,
        outcome => outcome,
    }
}

pub(crate) fn parse_css_value_with_utf16_source(
    context: &ParseContext,
    property_id: u16,
    values: &[ComponentValue],
    source: &[u16],
) -> ParseOutcome {
    if values
        .iter()
        .any(|value| matches!(value.kind, ComponentKind::Token(ParserTokenKind::Semicolon)))
    {
        return ParseOutcome::Invalid;
    }
    let mut substitution_presence = SubstitutionFunctionsPresence::default();
    if collect_arbitrary_substitution_function_presence(values, &mut substitution_presence).is_err() {
        return ParseOutcome::Invalid;
    }
    if property_id == property_id::CUSTOM {
        let comparison_source = crate::css::serialize::serialize_component_values_to_utf16(
            values,
            crate::css::parser::component_value::ComponentSerializationMode::Normalized,
        );
        return parse_css_value_after_substitution_scan(
            context,
            property_id,
            values,
            source,
            &comparison_source,
            substitution_presence,
        );
    }
    if substitution_presence.has_any() {
        let unresolved_source = crate::css::serialize::serialize_component_values_to_utf16(
            values,
            crate::css::parser::component_value::ComponentSerializationMode::PreserveNumericSource,
        );
        return parse_css_value_after_substitution_scan(
            context,
            property_id,
            values,
            &unresolved_source,
            &[],
            substitution_presence,
        );
    }
    parse_css_value_after_substitution_scan(context, property_id, values, &[], &[], substitution_presence)
}

/// Parse a property value using the grammars which have been ported to Rust.
pub(crate) fn parse_css_value(context: &ParseContext, property_id: u16, values: &[ComponentValue]) -> ParseOutcome {
    let source = values
        .iter()
        .flat_map(|value| value.original_source_text.iter())
        .collect::<Vec<_>>();
    parse_css_value_with_source(context, property_id, values, &source, &[])
}

fn component_values_from_source<'a>(
    source: impl Into<TokenizerInput<'a>>,
) -> Result<smallvec::SmallVec<[ComponentValue; 8]>, ()> {
    consume_a_small_list_of_component_values(tokenize_for_parser_without_source(source))
}

pub(crate) fn svg_path_strings_from_source(source: &[u16]) -> Vec<Vec<u16>> {
    fn collect(values: &[ComponentValue], paths: &mut Vec<Vec<u16>>) {
        for value in values {
            match &value.kind {
                ComponentKind::Function { name, values } => {
                    if equals_ascii_case_insensitive(name, b"path") {
                        for path in values.iter().filter_map(ComponentValue::string) {
                            if !paths.iter().any(|candidate| candidate == path) {
                                paths.push(path.to_vec());
                            }
                        }
                    }
                    collect(values, paths);
                }
                ComponentKind::SimpleBlock { values, .. } => collect(values, paths),
                ComponentKind::Token(_) => {}
            }
        }
    }

    let Ok(values) = component_values_from_source(source) else {
        return Vec::new();
    };
    let mut paths = Vec::new();
    collect(&values, &mut paths);
    paths
}

/// Parses a UTF-16 property value whose component-value source is already serialized.
pub(crate) fn parse_css_value_from_source(context: &ParseContext, property_id: u16, source: &[u16]) -> ParseOutcome {
    match component_values_from_source(source) {
        Ok(values) => parse_css_value_with_source(context, property_id, &values, source, &[]),
        Err(()) => ParseOutcome::NotHandled,
    }
}

/// The result category returned through the value-parser FFI.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FfiParseStatus {
    Parsed,
    Invalid,
    NotHandled,
}

#[derive(Clone, Copy)]
pub(crate) enum FontDescriptorKind {
    FamilyName,
    SourceList,
    UnicodeRangeList,
}

/// Tries the Rust value parser and returns a strong StyleValueData reference
/// on success. A null return is disambiguated by `out_status`.
///
/// # Safety
/// All non-null pointers must remain readable for their accompanying lengths
/// for the duration of this call. `out_status` must be a valid writable
/// pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_css_value(
    context: *const ParseContext,
    property_id: u16,
    source: FfiUtf16View,
    out_status: *mut FfiParseStatus,
) -> *const c_void {
    crate::abort_on_panic(|| {
        let invalid_ffi_result = || {
            if !out_status.is_null() {
                unsafe { *out_status = FfiParseStatus::NotHandled };
            }
            std::ptr::null()
        };

        if context.is_null() || out_status.is_null() {
            return invalid_ffi_result();
        }
        let Some(source) = (unsafe { source.units() }) else {
            return invalid_ffi_result();
        };
        let mut source_utf16 = Vec::with_capacity(source.len());
        source.append_to(&mut source_utf16);
        let context = unsafe { &*context };
        let outcome = match component_values_from_source(source) {
            Ok(values) => {
                let mut substitution_presence = SubstitutionFunctionsPresence::default();
                if collect_arbitrary_substitution_function_presence(&values, &mut substitution_presence).is_err() {
                    ParseOutcome::Invalid
                } else if property_id == property_id::CUSTOM || substitution_presence.has_any() {
                    match consume_a_list_of_component_values(tokenize_for_parser(source_utf16.as_slice())) {
                        Ok(values) => parse_css_value_with_utf16_source(context, property_id, &values, &source_utf16),
                        Err(()) => ParseOutcome::NotHandled,
                    }
                } else {
                    parse_css_value_after_substitution_scan(
                        context,
                        property_id,
                        &values,
                        &[],
                        &[],
                        substitution_presence,
                    )
                }
            }
            Err(()) => ParseOutcome::NotHandled,
        };
        match outcome {
            ParseOutcome::Parsed(value) => {
                unsafe {
                    *out_status = FfiParseStatus::Parsed;
                }
                Arc::into_raw(value).cast()
            }
            ParseOutcome::Invalid => {
                unsafe {
                    *out_status = FfiParseStatus::Invalid;
                }
                std::ptr::null()
            }
            ParseOutcome::NotHandled => {
                unsafe {
                    *out_status = FfiParseStatus::NotHandled;
                }
                std::ptr::null()
            }
        }
    })
}

/// Returns a substitution-function presence bitmap for CSS source.
///
/// # Safety
/// The source pointer must be valid for its accompanying length and
/// `out_presence` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_collect_arbitrary_substitution_function_presence_from_source(
    source: FfiUtf16View,
    out_presence: *mut u8,
) -> bool {
    crate::abort_on_panic(|| {
        if out_presence.is_null() {
            return false;
        }
        let Some(source) = (unsafe { source.units() }) else {
            return false;
        };
        let Ok(values) = component_values_from_source(source) else {
            return false;
        };
        let Some(presence) = substitution_function_presence_bits(&values) else {
            return false;
        };
        unsafe { *out_presence = presence };
        true
    })
}

fn parse_font_feature_values_from_source(source: TokenizerInput<'_>, maximum_value_count: usize) -> Option<Vec<u32>> {
    let values = component_values_from_source(source).ok()?;
    let values = values
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

/// Parses the non-negative integer list used by font feature value rules.
///
/// Returns `usize::MAX` for invalid input. Otherwise returns the value count and
/// writes it when the provided output has sufficient capacity.
///
/// # Safety
/// The source pointer must be valid for its accompanying length. A non-null
/// output pointer must be writable for `output_capacity` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_font_feature_values(
    source: FfiUtf16View,
    maximum_value_count: usize,
    output: *mut u32,
    output_capacity: usize,
) -> usize {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return usize::MAX;
        };
        let Some(values) = parse_font_feature_values_from_source(source, maximum_value_count) else {
            return usize::MAX;
        };
        if output_capacity >= values.len() {
            if output.is_null() {
                return usize::MAX;
            }
            unsafe { std::ptr::copy_nonoverlapping(values.as_ptr(), output, values.len()) };
        }
        values.len()
    })
}

type VisitMarginComponent = unsafe extern "C" fn(*mut c_void, u8, f64, *const u16, usize);

/// Visits the dimension and percentage components accepted by intersection margins.
///
/// # Safety
/// The source must remain readable during the call. The callback and context must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_visit_margin_components(
    source: FfiUtf16View,
    context: *mut c_void,
    visit: VisitMarginComponent,
) -> bool {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return false;
        };
        let Ok(values) = component_values_from_source(source) else {
            return false;
        };
        let values = values.iter().filter(|value| !value.is_whitespace()).collect::<Vec<_>>();
        if values.len() > 4
            || values.iter().any(|value| {
                !matches!(
                    value.kind,
                    ComponentKind::Token(ParserTokenKind::Dimension { .. } | ParserTokenKind::Percentage { .. })
                )
            })
        {
            return false;
        }
        for value in values {
            match &value.kind {
                ComponentKind::Token(ParserTokenKind::Dimension { value, unit, .. }) => unsafe {
                    visit(context, 0, *value, unit.as_ptr(), unit.len());
                },
                ComponentKind::Token(ParserTokenKind::Percentage { value, .. }) => unsafe {
                    visit(context, 1, *value, std::ptr::null(), 0);
                },
                _ => unreachable!(),
            }
        }
        true
    })
}

fn parse_css_primitive_values(
    context: &ParseContext,
    value_type: u8,
    values: &[ComponentValue],
    range_min: f64,
    range_max: f64,
) -> Option<(StyleValueData, usize)> {
    let mut stream = TokenStream::new(values);
    let range = NumericRange::new(range_min, range_max);
    let property = if context.value_context_count == 0 || context.value_contexts.is_null() {
        property_id::WIDTH
    } else {
        unsafe { std::slice::from_raw_parts(context.value_contexts, context.value_context_count) }
            .iter()
            .find(|value_context| {
                value_context.kind == FfiValueParsingContextKind::Property && value_context.value != property_id::CUSTOM
            })
            .map_or(property_id::WIDTH, |value_context| value_context.value)
    };
    let parsed = match value_type {
        VALUE_TYPE_ANGLE => parse_angle_from_stream(context, property, &mut stream, range),
        VALUE_TYPE_INTEGER => parse_integer_from_stream(context, property, &mut stream, range),
        VALUE_TYPE_LENGTH => parse_length_from_stream(context, property, &mut stream, range),
        VALUE_TYPE_LENGTH_PERCENTAGE => {
            parse_length_percentage_from_stream(context, property, &mut stream, range, range)
        }
        VALUE_TYPE_NUMBER => parse_number_from_stream(context, property, &mut stream, range),
        VALUE_TYPE_PERCENTAGE => {
            stream.discard_whitespace();
            let value = stream.next_token();
            let parsed = if let Some((name, arguments)) = value.function()
                && math_function_from_name(name).is_some()
            {
                parse_calculated_numeric_value_with_ranges(
                    context,
                    property,
                    VALUE_TYPE_PERCENTAGE,
                    None,
                    range,
                    name,
                    arguments,
                )
            } else {
                parse_percentage_value(value, range)
            }?;
            stream.discard_a_token();
            Some(parsed)
        }
        VALUE_TYPE_RESOLUTION => parse_resolution_from_stream(context, property, &mut stream, range),
        VALUE_TYPE_COLOR => crate::css::parser::color_parser::parse_color_value(context, property, &mut stream, false),
        VALUE_TYPE_STRING => {
            stream.discard_whitespace();
            let parsed = parse_string_value(context, stream.next_token())?;
            stream.discard_a_token();
            Some(parsed)
        }
        VALUE_TYPE_URL => {
            stream.discard_whitespace();
            let parsed = parse_url_value(context, stream.next_token())?;
            stream.discard_a_token();
            Some(parsed)
        }
        VALUE_TYPE_RATIO => {
            stream.discard_whitespace();
            let first = stream.consume_a_token().clone();
            stream.discard_whitespace();
            let parsed = if stream.next_token().is_delim(b'/') {
                let slash = stream.consume_a_token().clone();
                stream.discard_whitespace();
                let second = stream.consume_a_token().clone();
                parse_ratio_value_with_context(context, property, &[&first, &slash, &second])
            } else {
                parse_ratio_value_with_context(context, property, &[&first])
            }?;
            Some(parsed)
        }
        VALUE_TYPE_FLEX | VALUE_TYPE_FREQUENCY | VALUE_TYPE_TIME => {
            stream.discard_whitespace();
            let value = stream.next_token();
            let parsed = if let Some((name, arguments)) = value.function()
                && math_function_from_name(name).is_some()
            {
                parse_calculated_numeric_value_with_ranges(context, property, value_type, None, range, name, arguments)
            } else {
                match value_type {
                    VALUE_TYPE_FLEX => parse_flex_value(value, range),
                    VALUE_TYPE_FREQUENCY => parse_frequency_value(value, range),
                    VALUE_TYPE_TIME => parse_time_value(value, range),
                    _ => unreachable!(),
                }
            }?;
            stream.discard_a_token();
            Some(parsed)
        }
        _ => None,
    }?;
    Some((parsed, stream.current_index()))
}

/// Parses an entire CSS source as one value of a primitive CSS value type.
///
/// # Safety
/// All pointers must be valid for their accompanying lengths.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_entire_css_primitive_from_source(
    context: *const ParseContext,
    value_type: u8,
    source: FfiUtf16View,
    range_min: f64,
    range_max: f64,
) -> *const c_void {
    crate::abort_on_panic(|| {
        if context.is_null() {
            return std::ptr::null();
        }
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null();
        };
        let Ok(values) = component_values_from_source(source) else {
            return std::ptr::null();
        };
        let Some((parsed, consumed)) =
            parse_css_primitive_values(unsafe { &*context }, value_type, &values, range_min, range_max)
        else {
            return std::ptr::null();
        };
        if values[consumed..].iter().all(ComponentValue::is_whitespace) {
            Arc::into_raw(Arc::new(parsed)).cast()
        } else {
            std::ptr::null()
        }
    })
}

/// Parses an entire CSS source as one known keyword, or returns `u16::MAX`.
///
/// # Safety
/// All pointers must be valid for their accompanying lengths.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_css_keyword_from_source(source: FfiUtf16View) -> u16 {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return u16::MAX;
        };
        let Ok(values) = component_values_from_source(source) else {
            return u16::MAX;
        };
        single_non_whitespace_value(&values)
            .and_then(ComponentValue::ident)
            .and_then(keyword_from_ascii_case_insensitive)
            .unwrap_or(u16::MAX)
    })
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

    #[test]
    fn parses_font_feature_value_integer_lists() {
        assert_eq!(
            parse_font_feature_values_from_source(utf16(" 1 +2 0").as_slice().into(), 3),
            Some(vec![1, 2, 0])
        );
        assert!(parse_font_feature_values_from_source(utf16("1 2").as_slice().into(), 1).is_none());
        for source in ["", "-1", "1.5", "1 ident"] {
            assert!(
                parse_font_feature_values_from_source(utf16(source).as_slice().into(), usize::MAX).is_none(),
                "{source}"
            );
        }
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

    fn parse(property: u16, source: &str) -> ParseOutcome {
        let source = utf16(source);
        let values = consume_a_list_of_component_values(tokenize_for_parser(&source)).unwrap();
        parse_css_value(&context(), property, &values)
    }

    fn parse_with_context(context: &ParseContext, property: u16, source: &str) -> ParseOutcome {
        let source = utf16(source);
        let values = consume_a_list_of_component_values(tokenize_for_parser(&source)).unwrap();
        parse_css_value(context, property, &values)
    }

    #[test]
    fn parses_a_serialized_substituted_value() {
        let mut parse_context = context();
        parse_context.is_substituted_value = true;
        let valid = utf16("rgb(1 2 3)");
        let trailing = utf16("rgb(1 2 3) trailing");
        assert!(matches!(
            parse_css_value_from_source(&parse_context, property_id::COLOR, &valid),
            ParseOutcome::Parsed(_)
        ));
        assert!(matches!(
            parse_css_value_from_source(&parse_context, property_id::COLOR, &trailing),
            ParseOutcome::Invalid
        ));
    }

    fn component(source: &str) -> ComponentValue {
        let source = utf16(source);
        let mut values = consume_a_list_of_component_values(tokenize_for_parser(&source)).unwrap();
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
    fn unknown_properties_fall_back_to_cpp() {
        let values = consume_a_list_of_component_values(tokenize_for_parser(b"0.5")).unwrap();
        assert!(matches!(
            parse_css_value(&context(), property_id::ALL, &values),
            ParseOutcome::Invalid
        ));
        assert!(matches!(
            parse_css_value(&context(), u16::MAX, &values),
            ParseOutcome::NotHandled
        ));
    }

    #[test]
    fn parses_positional_shorthands() {
        for (property, source, expected_values) in [
            (property_id::PADDING, "1px 2px 3px", 4),
            (property_id::GAP, "normal 2%", 2),
            (property_id::OVERFLOW, "hidden auto", 2),
            (property_id::BORDER_COLOR, "red green", 4),
            (property_id::MARGIN_INLINE, "auto 1px", 2),
        ] {
            let ParseOutcome::Parsed(value) = parse(property, source) else {
                panic!("shorthand should parse: {source}");
            };
            let StyleValueData::Shorthand {
                shorthand_property,
                sub_properties,
                values,
            } = &*value
            else {
                panic!("value should be a shorthand: {source}");
            };
            assert_eq!(*shorthand_property, property);
            assert_eq!(sub_properties.as_slice(), longhands_for_shorthand(property));
            assert_eq!(values.as_slice().len(), expected_values);
        }
    }

    #[test]
    fn parses_unordered_shorthands_and_fills_initial_values() {
        for (property, source) in [
            (property_id::BORDER_TOP, "thick dotted red"),
            (property_id::BORDER_INLINE_START, "blue"),
            (property_id::FLEX_FLOW, "wrap column"),
            (property_id::OUTLINE, "2px red"),
        ] {
            let ParseOutcome::Parsed(value) = parse(property, source) else {
                panic!("shorthand should parse: {source}");
            };
            let StyleValueData::Shorthand {
                sub_properties, values, ..
            } = &*value
            else {
                panic!("value should be a shorthand: {source}");
            };
            if property == property_id::FLEX_FLOW {
                assert_eq!(sub_properties.as_slice(), longhands_for_shorthand(property));
            }
            assert_eq!(values.as_slice().len(), longhands_for_shorthand(property).len());
        }
        for (property, source) in [
            (property_id::BORDER_TOP, "red blue"),
            (property_id::FLEX_FLOW, "wrap nowrap"),
            (property_id::OUTLINE, "initial solid"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn parses_composite_border_flex_and_columns_shorthands() {
        for (property, source, expected_properties) in [
            (property_id::BORDER, "solid red", 4),
            (property_id::BORDER_BLOCK, "2px dashed", 3),
            (property_id::FLEX, "2 3 10%", 3),
            (property_id::FLEX, "none", 3),
            (property_id::COLUMNS, "20em 3 / 40em", 3),
            (property_id::COLUMNS, "auto", 3),
        ] {
            let ParseOutcome::Parsed(value) = parse(property, source) else {
                panic!("shorthand should parse: {source}");
            };
            let StyleValueData::Shorthand {
                sub_properties, values, ..
            } = &*value
            else {
                panic!("value should be a shorthand: {source}");
            };
            assert_eq!(sub_properties.as_slice().len(), expected_properties);
            assert_eq!(values.as_slice().len(), expected_properties);
        }
        for (property, source) in [
            (property_id::BORDER, "solid dashed"),
            (property_id::FLEX, "none 1"),
            (property_id::COLUMNS, "/ auto"),
            (property_id::COLUMNS, "1 / bogus"),
            (property_id::COLUMNS, "10px 20px"),
            (property_id::COLUMNS, "initial initial"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn parses_grouping_shorthands() {
        for (property, source) in [
            (property_id::CONTAINER, "main / inline-size"),
            (property_id::PLACE_CONTENT, "space-between center"),
            (property_id::PLACE_ITEMS, "center"),
            (property_id::PLACE_SELF, "start end"),
            (property_id::LIST_STYLE, "inside square"),
            (property_id::LIST_STYLE, "none none"),
            (property_id::TEXT_WRAP, "balance"),
            (property_id::WHITE_SPACE, "pre-wrap"),
            (property_id::WHITE_SPACE, "preserve nowrap discard-after"),
            (property_id::WHITE_SPACE, "discard-inner preserve"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        for (property, source) in [
            (property_id::CONTAINER, "main /"),
            (property_id::PLACE_ITEMS, "normal normal normal"),
            (property_id::LIST_STYLE, "none none none"),
            (property_id::WHITE_SPACE, "wrap nowrap"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn parses_decoration_clip_and_timeline_shorthands() {
        for (property, source) in [
            (property_id::OVERFLOW_CLIP_MARGIN, "content-box 2px"),
            (property_id::TEXT_DECORATION, "underline overline wavy red 2px"),
            (property_id::SCROLL_TIMELINE, "--main inline, none"),
            (property_id::VIEW_TIMELINE, "--main 10% 20% inline, none"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        for (property, source) in [
            (property_id::OVERFLOW_CLIP_MARGIN, "1px 2px"),
            (property_id::TEXT_DECORATION, "solid dashed"),
            (property_id::SCROLL_TIMELINE, "--main inline block"),
            (property_id::VIEW_TIMELINE, "--main, "),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn parses_border_image_shorthand() {
        for source in [
            "none",
            "url(border.png) 30 fill / 10px / 2 round",
            "10% / / 1 repeat",
            "stretch 20%",
        ] {
            let ParseOutcome::Parsed(value) = parse(property_id::BORDER_IMAGE, source) else {
                panic!("shorthand should parse: {source}");
            };
            let StyleValueData::Shorthand { values, .. } = &*value else {
                panic!("value should be a shorthand: {source}");
            };
            assert_eq!(values.as_slice().len(), 5);
        }
        for source in ["10 /", "10 / /", "10 / 2 /", "round round round"] {
            assert!(matches!(
                parse(property_id::BORDER_IMAGE, source),
                ParseOutcome::Invalid
            ));
        }
        assert!(matches!(
            parse(property_id::BORDER_IMAGE, "1 / none / 1px"),
            ParseOutcome::Invalid
        ));
    }

    #[test]
    fn parses_font_shorthands() {
        for (property, source, expected_values) in [
            (property_id::FONT_VARIANT, "normal", 7),
            (property_id::FONT_VARIANT, "none", 7),
            (property_id::FONT_VARIANT, "small-caps oldstyle-nums ruby emoji", 7),
            (property_id::FONT, "16px serif", 12),
            (
                property_id::FONT,
                "italic small-caps 700 condensed 12px/1.5 'Example', serif",
                12,
            ),
            (property_id::FONT, "normal normal 10pt sans-serif", 12),
            (property_id::FONT, "oblique 45deg 24px Arial", 12),
            (property_id::FONT, "16px initial simple", 12),
        ] {
            let ParseOutcome::Parsed(value) = parse(property, source) else {
                panic!("shorthand should parse: {source}");
            };
            let StyleValueData::Shorthand { values, .. } = &*value else {
                panic!("value should be a shorthand: {source}");
            };
            assert_eq!(values.as_slice().len(), expected_values);
        }
        for (property, source) in [
            (property_id::FONT_VARIANT, "small-caps all-small-caps"),
            (property_id::FONT_VARIANT, "normal small-caps"),
            (property_id::FONT, "italic 16px"),
            (property_id::FONT, "16px initial"),
            (property_id::FONT, "normal normal normal normal normal 16px serif"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn parses_background_and_mask_shorthands() {
        for (property, source, expected_values) in [
            (property_id::BACKGROUND, "red", 8),
            (
                property_id::BACKGROUND,
                "url(a.png) left top / cover no-repeat fixed padding-box content-box",
                8,
            ),
            (
                property_id::BACKGROUND,
                "none, linear-gradient(red, blue) center / 20px 30px",
                8,
            ),
            (property_id::MASK, "none", 8),
            (
                property_id::MASK,
                "url(mask.png) center / contain no-repeat border-box no-clip add luminance",
                8,
            ),
            (property_id::MASK, "none, linear-gradient(black, transparent)", 8),
        ] {
            let ParseOutcome::Parsed(value) = parse(property, source) else {
                panic!("shorthand should parse: {source}");
            };
            let StyleValueData::Shorthand { values, .. } = &*value else {
                panic!("value should be a shorthand: {source}");
            };
            assert_eq!(values.as_slice().len(), expected_values);
        }
        for (property, source) in [
            (property_id::BACKGROUND, "red, none"),
            (property_id::BACKGROUND, "center /"),
            (property_id::MASK, "center /"),
            (property_id::MASK, "none,"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn parses_animation_and_transition_shorthands() {
        for (property, source) in [
            (
                property_id::ANIMATION,
                "slide 2s ease-in 1s infinite alternate both paused",
            ),
            (property_id::ANIMATION, "none, fade 200ms"),
            (property_id::TRANSITION, "opacity 1s ease 200ms allow-discrete"),
            (property_id::TRANSITION, "color 1s, opacity 2s linear"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        for (property, source) in [
            (property_id::ANIMATION, "1s 2s 3s"),
            (property_id::TRANSITION, "none 1s, opacity 2s"),
            (property_id::TRANSITION, "opacity ease linear"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
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
    fn parses_mixed_and_special_grammars() {
        assert!(matches!(
            parse(property_id::ALIGN_ITEMS, "normal"),
            ParseOutcome::Parsed(_)
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
            ParseOutcome::Invalid
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
            ParseOutcome::Parsed(value) if matches!(&*value, StyleValueData::Unresolved { presence_var: true, .. })
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
            (property_id::CONTENT, "\"\" \"\" \"\""),
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
            ParseOutcome::Invalid
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
        assert!(matches!(
            parse(property_id::ANCHOR_NAME, "var(--anchor)"),
            ParseOutcome::Parsed(value) if matches!(&*value, StyleValueData::Unresolved { presence_var: true, .. })
        ));
    }

    #[test]
    fn parses_special_keyword_combinations() {
        for (property, source) in [
            (property_id::CONTAIN, "paint inline-size layout"),
            (property_id::CONTAINER_TYPE, "scroll-state size"),
            (property_id::POSITION_VISIBILITY, "no-overflow anchors-valid"),
            (property_id::TEXT_DECORATION_LINE, "overline underline"),
            (property_id::TOUCH_ACTION, "pinch-zoom pan-y pan-left"),
            (property_id::WHITE_SPACE_TRIM, "discard-inner discard-before"),
            (property_id::PAINT_ORDER, "markers stroke fill"),
            (property_id::SCROLLBAR_GUTTER, "both-edges stable"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        for (property, source) in [
            (property_id::CONTAIN, "size inline-size"),
            (property_id::CONTAINER_TYPE, "normal size"),
            (property_id::POSITION_VISIBILITY, "anchors-valid anchors-valid"),
            (property_id::TEXT_DECORATION_LINE, "spelling-error underline"),
            (property_id::TOUCH_ACTION, "pan-left pan-right"),
            (property_id::WHITE_SPACE_TRIM, "none discard-inner"),
            (property_id::PAINT_ORDER, "fill fill"),
            (property_id::SCROLLBAR_GUTTER, "both-edges"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn parses_special_numeric_longhands() {
        for (property, source) in [
            (property_id::ASPECT_RATIO, "auto 16 / 9"),
            (property_id::MATH_DEPTH, "add(2)"),
            (property_id::SCROLLBAR_COLOR, "red blue"),
            (property_id::STROKE_DASHARRAY, "1 2px, 3%"),
            (property_id::TEXT_INDENT, "hanging 2em each-line"),
            (property_id::TEXT_UNDERLINE_POSITION, "under right"),
            (property_id::OVERFLOW_CLIP_MARGIN_TOP, "content-box 2px"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        let mut random_function_index = 0;
        let mut random_context = context();
        let value_context = FfiValueParsingContext {
            kind: FfiValueParsingContextKind::Property,
            value: property_id::MATH_DEPTH,
            secondary_value: 0,
            name: FfiUtf16View::default(),
        };
        random_context.value_contexts = &value_context;
        random_context.value_context_count = 1;
        random_context.random_function_index = &mut random_function_index;
        assert!(matches!(
            parse_with_context(
                &random_context,
                property_id::MATH_DEPTH,
                "random(fixed calc(2 / 4), 0, 10)"
            ),
            ParseOutcome::Parsed(_)
        ));
        for (property, source) in [
            (property_id::ASPECT_RATIO, "auto auto"),
            (property_id::MATH_DEPTH, "add(1 2)"),
            (property_id::SCROLLBAR_COLOR, "red"),
            (property_id::STROKE_DASHARRAY, "-1"),
            (property_id::TEXT_INDENT, "hanging"),
            (property_id::TEXT_UNDERLINE_POSITION, "left right"),
            (property_id::OVERFLOW_CLIP_MARGIN_TOP, "margin-box"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn parses_alignment_grid_and_paint_longhands() {
        for (property, source) in [
            (property_id::ALIGN_ITEMS, "normal"),
            (property_id::ALIGN_SELF, "auto"),
            (property_id::JUSTIFY_ITEMS, "legacy"),
            (property_id::JUSTIFY_SELF, "baseline"),
            (property_id::GRID_AUTO_FLOW, "dense column"),
            (property_id::POSITION, "sticky"),
            (property_id::FILL, "red"),
            (property_id::STROKE, "url(#paint) none"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        for (property, source) in [
            (property_id::ALIGN_ITEMS, "safe"),
            (property_id::JUSTIFY_ITEMS, "legacy right"),
            (property_id::GRID_AUTO_FLOW, "row column"),
            (property_id::POSITION, "bogus"),
            (property_id::FILL, "url(#paint) red blue"),
            (property_id::POSITION_TRY_FALLBACKS, "flip-inline --bar flip-block"),
            (property_id::BORDER_IMAGE_SLICE, "1% fill 2%"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }

        let ParseOutcome::Parsed(value) = parse(property_id::FILL, "url(#paint)") else {
            panic!("URL paint should parse");
        };
        let StyleValueData::ValueList { values, .. } = value.as_ref() else {
            panic!("URL paint should be a value list");
        };
        assert_eq!(values.as_slice().len(), 2);
        assert!(matches!(values.as_slice()[1].data(), StyleValueData::EmptyOptional));
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
            ParseOutcome::Parsed(_)
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
        assert!(matches!(parse(property_id::WIDTH, "-1px"), ParseOutcome::Invalid));
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
    fn parses_background_repeat_size_and_border_slices() {
        for (property, source) in [
            (property_id::BACKGROUND_REPEAT, "repeat-x, space round"),
            (property_id::MASK_REPEAT, "no-repeat repeat"),
            (property_id::BACKGROUND_SIZE, "cover, 10px auto"),
            (property_id::MASK_SIZE, "25%"),
            (property_id::BORDER_IMAGE_SLICE, "fill 10% 20 30% 40"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        for (property, source) in [
            (property_id::BACKGROUND_REPEAT, "repeat-x round"),
            (property_id::MASK_SIZE, "cover auto"),
            (property_id::BORDER_IMAGE_SLICE, "fill fill 10"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn parses_shadow_longhands() {
        for (property, source) in [
            (property_id::BOX_SHADOW, "none"),
            (property_id::BOX_SHADOW, "inset red 1px 2px 3px -4px, 0 0 blue"),
            (property_id::TEXT_SHADOW, "1px 2px, rgb(1 2 3) 0 0 4px"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        for (property, source) in [
            (property_id::BOX_SHADOW, "1px"),
            (property_id::BOX_SHADOW, "1px 2px -3px"),
            (property_id::TEXT_SHADOW, "inset 1px 2px"),
            (property_id::TEXT_SHADOW, "1px 2px 3px 4px"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn parses_remaining_special_position_longhands() {
        for (property, source) in [
            (property_id::D, "none"),
            (property_id::D, "path('M 0 0 L 1 1')"),
            (property_id::POSITION_AREA, "left span-all"),
            (property_id::POSITION_AREA, "bottom right"),
            (
                property_id::POSITION_TRY_FALLBACKS,
                "--wide flip-block flip-inline, top",
            ),
            (property_id::TRANSFORM_ORIGIN, "top"),
            (property_id::TRANSFORM_ORIGIN, "right 25% calc(1px + 2px)"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        for (property, source) in [
            (property_id::D, "circle()"),
            (property_id::POSITION_AREA, "left right"),
            (property_id::POSITION_TRY_FALLBACKS, "--wide --narrow"),
            (property_id::TRANSFORM_ORIGIN, "10px left"),
            (property_id::TRANSFORM_ORIGIN, "left right"),
            (property_id::TRANSFORM_ORIGIN, "left top 10%"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
        }
    }

    #[test]
    fn every_longhand_initial_value_is_parsed_authoritatively() {
        for property in FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID {
            let initial_value = property_initial_value(property);
            assert!(
                matches!(parse(property, initial_value), ParseOutcome::Parsed(_)),
                "Rust parser did not handle the C++ initial value for {}: {initial_value}",
                property_name(property)
            );
        }
    }

    #[test]
    fn parses_space_separated_generic_property_values() {
        let ParseOutcome::Parsed(value) = parse(property_id::BORDER_SPACING, "1px 2px") else {
            panic!("border-spacing should parse");
        };
        assert!(
            matches!(&*value, StyleValueData::ValueList { values, separator: 0, .. } if values.as_slice().len() == 2)
        );
        assert!(matches!(
            parse(property_id::BORDER_SPACING, "1px 2px 3px"),
            ParseOutcome::Invalid
        ));
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
        let values = consume_a_list_of_component_values(tokenize_for_parser(b"16 / 9"))
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
            (
                property_id::ANIMATION_ITERATION_COUNT,
                "calc(sibling-index() + sign(1em - 1px))",
            ),
            (property_id::FILTER, "brightness(sibling-count())"),
            (property_id::GRID_ROW_START, "span sibling-count()"),
            (property_id::FONT_VARIATION_SETTINGS, "\"wght\" sibling-index()"),
            (property_id::FONT_STYLE, "oblique calc(5deg * sibling-index())"),
            (property_id::TRANSFORM, "matrix(sibling-index(), 2, 3, 4, 5, 6)"),
            (
                property_id::TRANSITION_TIMING_FUNCTION,
                "steps(sibling-index(), jump-none)",
            ),
            (
                property_id::ANIMATION_TIMING_FUNCTION,
                "cubic-bezier(0, sibling-index(), 1, sign(2em - 20px))",
            ),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        assert!(matches!(
            parse(property_id::WIDTH, "calc(anchor-size(width) + 1px)"),
            ParseOutcome::Invalid
        ));
    }

    #[test]
    fn rejects_random_in_canvas_colors() {
        let mut random_function_index = 0;
        let value_contexts = [
            FfiValueParsingContext {
                kind: FfiValueParsingContextKind::Special,
                value: 0,
                secondary_value: 0,
                name: FfiUtf16View::default(),
            },
            FfiValueParsingContext {
                kind: FfiValueParsingContextKind::Property,
                value: property_id::COLOR,
                secondary_value: 0,
                name: FfiUtf16View::default(),
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
            ParseOutcome::Invalid
        ));
        assert_eq!(random_function_index, 0);
    }

    #[test]
    fn parses_substitution_functions_as_unresolved_values() {
        for (property, source, expected_attr, expected_var) in [
            (property_id::OPACITY, "var(--opacity)", false, true),
            (
                property_id::FONT_SIZE,
                "attr(data-size type(<percentage>))",
                true,
                false,
            ),
            (property_id::BACKGROUND_IMAGE, "image(attr(data-foo))", true, false),
        ] {
            let ParseOutcome::Parsed(value) = parse(property, source) else {
                panic!("substitution function should parse: {source}");
            };
            let StyleValueData::Unresolved {
                source_text,
                value_comparison_text,
                presence_attr,
                presence_var,
                ..
            } = &*value
            else {
                panic!("substitution function should produce an unresolved value: {source}");
            };
            assert_eq!(retained_utf16(source_text), utf16(source));
            assert!(value_comparison_text.as_units().is_empty());
            assert_eq!(*presence_attr, expected_attr);
            assert_eq!(*presence_var, expected_var);
        }
        assert!(matches!(
            parse(property_id::Z_INDEX, "sibling-index()"),
            ParseOutcome::Parsed(_)
        ));
        assert!(matches!(
            parse(property_id::FILTER, "blur(random(10px, 20px))"),
            ParseOutcome::Invalid
        ));
        for source in ["var()", "attr()", "env()", "inherit()", "if()", "--mix(, value)"] {
            assert!(matches!(parse(property_id::WIDTH, source), ParseOutcome::Invalid));
        }

        let source = utf16("  attr(    foo    )  ");
        let values = consume_a_list_of_component_values(tokenize_for_parser(&source)).unwrap();
        let ParseOutcome::Parsed(value) =
            parse_css_value_with_utf16_source(&context(), property_id::CONTENT, &values, &source)
        else {
            panic!("attr() should parse");
        };
        let StyleValueData::Unresolved {
            source_text,
            value_comparison_text,
            ..
        } = &*value
        else {
            panic!("attr() should remain unresolved");
        };
        assert_eq!(retained_utf16(source_text), utf16("attr( foo )"));
        assert!(value_comparison_text.as_units().is_empty());
    }

    #[test]
    fn parses_custom_properties_as_unresolved_values() {
        let source = utf16(" 10\\70 x ");
        let comparison = utf16("10px");
        let values = consume_a_list_of_component_values(tokenize_for_parser(&source)).unwrap();
        let ParseOutcome::Parsed(value) =
            parse_css_value_with_source(&context(), property_id::CUSTOM, &values, &source, &comparison)
        else {
            panic!("custom property should parse");
        };
        let StyleValueData::Unresolved {
            components,
            source_text,
            value_comparison_text,
            ..
        } = &*value
        else {
            panic!("custom property should produce an unresolved value");
        };
        assert_eq!(retained_utf16(source_text), utf16("10\\70 x"));
        assert_eq!(retained_utf16(value_comparison_text), comparison);
        assert_eq!(
            crate::css::serialize::serialize_component_values_to_utf16(
                components.as_slice(),
                crate::css::parser::component_value::ComponentSerializationMode::Normalized,
            ),
            comparison
        );

        let source = utf16("a/* comment */");
        let comparison = utf16("a");
        let values = consume_a_list_of_component_values(tokenize_for_parser(&source)).unwrap();
        let ParseOutcome::Parsed(value) =
            parse_css_value_with_source(&context(), property_id::CUSTOM, &values, &source, &comparison)
        else {
            panic!("custom property should parse");
        };
        let StyleValueData::Unresolved { components, .. } = &*value else {
            panic!("custom property should produce an unresolved value");
        };
        assert_eq!(
            crate::css::serialize::serialize_component_values_to_utf16(
                components.as_slice(),
                crate::css::parser::component_value::ComponentSerializationMode::Normalized,
            ),
            comparison
        );
        assert!(matches!(
            parse(property_id::CUSTOM, "inherit"),
            ParseOutcome::Parsed(value) if matches!(&*value, StyleValueData::Keyword { keyword: keyword::INHERIT })
        ));
        assert!(matches!(
            parse(property_id::CUSTOM, "value; trailing"),
            ParseOutcome::Invalid
        ));
    }

    #[test]
    fn restores_substitution_function_wrappers_from_context() {
        assert!(matches!(
            parse(property_id::TRANSFORM, "translateX(calc(-100px * -1))"),
            ParseOutcome::Parsed(_)
        ));
        for (property, function_names, source) in [
            (property_id::COLOR, vec![b"rgb".as_slice()], "1, 2, 3"),
            (
                property_id::TRANSFORM,
                vec![b"translateX".as_slice(), b"calc".as_slice()],
                "-100px * -1",
            ),
        ] {
            let mut function_contexts = vec![FfiValueParsingContext {
                kind: FfiValueParsingContextKind::Property,
                value: property,
                secondary_value: 0,
                name: FfiUtf16View::default(),
            }];
            function_contexts.extend(function_names.iter().map(|function_name| FfiValueParsingContext {
                kind: FfiValueParsingContextKind::Function,
                value: 0,
                secondary_value: 0,
                name: FfiUtf16View {
                    ascii: function_name.as_ptr(),
                    utf16: std::ptr::null(),
                    length: function_name.len(),
                },
            }));
            let mut parse_context = context();
            parse_context.value_contexts = function_contexts.as_ptr();
            parse_context.value_context_count = function_contexts.len();
            assert!(
                matches!(
                    parse_with_context(&parse_context, property, source),
                    ParseOutcome::Parsed(_)
                ),
                "failed to restore {function_names:?} around {source}"
            );
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
            let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
            let ParseOutcome::Parsed(value) = parse_css_value(&context(), property_id::WIDTH, &values) else {
                panic!("CSS-wide keyword should parse");
            };
            assert!(matches!(&*value, StyleValueData::Keyword { keyword } if *keyword == expected_keyword));
        }
    }

    #[test]
    fn does_not_parse_css_wide_keywords_as_partial_values() {
        let values = consume_a_list_of_component_values(tokenize_for_parser(b"inherit extra")).unwrap();
        assert!(matches!(
            parse_css_value(&context(), property_id::ALL, &values),
            ParseOutcome::Invalid
        ));
    }
}
