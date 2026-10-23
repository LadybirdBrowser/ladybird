/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The calc-size() function.
//! https://drafts.csswg.org/css-values-5/#calc-size

use crate::css::css_enums::{keyword, keyword_from_ascii_case_insensitive};
use crate::css::parser::component_value::{ComponentValue, trim_whitespace};
use crate::css::parser::value_parser::{
    NumericRange, ParseContext, equals_ascii_case_insensitive, parse_calculated_numeric_value_allowing_size_keyword,
    value_list,
};
use crate::css::property_metadata::{property_accepted_keywords, property_accepted_value_types};
use crate::css::serialize::{StringUnits, with_fly_string_units};
use crate::css::style_value::{CssString, RetainedStyleValueData, StyleValueData};
use crate::css::value_codes::{VALUE_TYPE_FIT_CONTENT, VALUE_TYPE_LENGTH};

const COMMA: u8 = 1;

pub(crate) fn is_calc_size_function_name(name: &CssString) -> bool {
    with_fly_string_units(name, |units| match units {
        StringUnits::Ascii(bytes) => bytes == b"calc-size",
        StringUnits::Utf16(units) => units.iter().copied().eq(b"calc-size".iter().copied().map(u16::from)),
    })
}

/// Parses `value` as a calc-size() function of the sizing property `property`.
pub(crate) fn parse_calc_size(context: &ParseContext, property: u16, value: &ComponentValue) -> Option<StyleValueData> {
    // <calc-size()> = calc-size( <calc-size-basis>, <calc-sum> )
    let (name, arguments) = value.function()?;
    if !equals_ascii_case_insensitive(name, b"calc-size") {
        return None;
    }
    let mut arguments = arguments.split(ComponentValue::is_comma);
    let basis = trim_whitespace(arguments.next()?);
    let calculation = trim_whitespace(arguments.next()?);
    if arguments.next().is_some() || basis.is_empty() || calculation.is_empty() {
        return None;
    }
    let basis = parse_basis(context, property, basis)?;
    // If the basis is any, it is a syntax error for size to appear in the calc-size calculation.
    let basis_is_any = matches!(basis, StyleValueData::Keyword { keyword: keyword::ANY });
    let calculation = parse_calc_sum(context, property, calculation, !basis_is_any)?;
    Some(calc_size_function(basis, calculation))
}

/// The calc-size() function value with the given basis and calculation.
pub(crate) fn calc_size_function(basis: StyleValueData, calculation: StyleValueData) -> StyleValueData {
    StyleValueData::Function {
        name: CssString::from_utf16(&"calc-size".encode_utf16().collect::<Vec<_>>()),
        value: RetainedStyleValueData::from_owned(value_list(vec![basis, calculation], COMMA, false)),
    }
}

// <calc-size-basis> = [ <size-keyword> | <calc-sum> | <calc-size()> | any ]
fn parse_basis(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    if let [value] = values {
        if let Some(identifier) = value.ident() {
            let keyword = keyword_from_ascii_case_insensitive(identifier)?;
            // <size-keyword> represents any sizing keywords allowed in the context where the function is used.
            let is_size_keyword = match keyword {
                keyword::ANY => true,
                keyword::AUTO | keyword::CONTENT | keyword::MIN_CONTENT | keyword::MAX_CONTENT | keyword::STRETCH => {
                    property_accepted_keywords(property).contains(&keyword)
                }
                keyword::FIT_CONTENT => property_accepted_value_types(property).contains(&VALUE_TYPE_FIT_CONTENT),
                _ => false,
            };
            return is_size_keyword.then_some(StyleValueData::Keyword { keyword });
        }
        if value
            .function()
            .is_some_and(|(name, _)| equals_ascii_case_insensitive(name, b"calc-size"))
        {
            return parse_calc_size(context, property, value);
        }
    }
    parse_calc_sum(context, property, values, false)
}

/// A <calc-sum> given without a calc() around it, which must match <length-percentage>.
fn parse_calc_sum(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
    allow_size_keyword: bool,
) -> Option<StyleValueData> {
    let name = "calc".encode_utf16().collect::<Vec<_>>();
    parse_calculated_numeric_value_allowing_size_keyword(
        context,
        property,
        VALUE_TYPE_LENGTH,
        Some(VALUE_TYPE_LENGTH),
        NumericRange::INFINITE,
        &name,
        values,
        allow_size_keyword,
    )
}

/// The basis and calculation of a calc-size() function value.
pub(crate) fn calc_size_arguments(value: &StyleValueData) -> Option<(&StyleValueData, &StyleValueData)> {
    let StyleValueData::Function { name, value } = value else {
        return None;
    };
    if !is_calc_size_function_name(name) {
        return None;
    }
    let StyleValueData::ValueList { values, .. } = value.data() else {
        return None;
    };
    match values.as_slice() {
        [basis, calculation] => Some((basis.data(), calculation.data())),
        _ => None,
    }
}

/// The sizing keyword a calc-size() acts as, after following nested calc-size() bases: auto, min-content,
/// max-content, fit-content or stretch, or none when its basis is any or a calculation.
pub(crate) fn calc_size_basis_keyword(value: &StyleValueData) -> Option<u16> {
    let (basis, _) = calc_size_arguments(value)?;
    match basis {
        StyleValueData::Keyword { keyword: keyword::ANY } => None,
        StyleValueData::Keyword { keyword } => Some(*keyword),
        basis @ StyleValueData::Function { .. } => calc_size_basis_keyword(basis),
        _ => None,
    }
}

/// Whether a calc-size() has a <calc-sum> basis, after following nested calc-size() bases.
pub(crate) fn calc_size_has_calc_sum_basis(value: &StyleValueData) -> bool {
    match calc_size_arguments(value) {
        Some((StyleValueData::Keyword { .. }, _)) | None => false,
        Some((basis @ StyleValueData::Function { .. }, _)) => calc_size_has_calc_sum_basis(basis),
        Some(_) => true,
    }
}

/// Resolves a calc-size() to pixels at used-value time.
/// `keyword_size` gives the used size of a sizing keyword basis, and percentages resolve against
/// `percentage_basis`, or against zero when that is indefinite.
/// https://drafts.csswg.org/css-values-5/#resolving-calc-size
pub(crate) fn resolve_calc_size(
    value: &StyleValueData,
    percentage_basis: Option<f64>,
    keyword_size: &dyn Fn(u16) -> f64,
) -> Option<f64> {
    let (basis, calculation) = calc_size_arguments(value)?;
    let size = match basis {
        StyleValueData::Keyword { keyword: keyword::ANY } => 0.0,
        StyleValueData::Keyword { keyword } => keyword_size(*keyword),
        basis @ StyleValueData::Function { .. } => resolve_calc_size(basis, percentage_basis, keyword_size)?,
        basis => resolve_length_percentage(basis, percentage_basis)?,
    };
    match calculation {
        calculation @ StyleValueData::Calculated { .. } => {
            crate::css::calc::resolve_calc_size_calculation(calculation, size, percentage_basis)
        }
        calculation => resolve_length_percentage(calculation, percentage_basis),
    }
}

fn resolve_length_percentage(value: &StyleValueData, percentage_basis: Option<f64>) -> Option<f64> {
    match value {
        StyleValueData::Length { value, .. } => Some(*value),
        StyleValueData::Percentage { value } => Some(percentage_basis.unwrap_or(0.0) * value / 100.0),
        value @ StyleValueData::Calculated { .. } => {
            crate::css::calc::resolve_calc_size_calculation(value, 0.0, percentage_basis)
        }
        _ => None,
    }
}
