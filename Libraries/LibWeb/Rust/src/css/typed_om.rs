/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the objects of the CSS Typed OM are built from and turned into, without the objects themselves, which live in
//! the bindings: units, numeric types and sum values, and the reification of style values.
//! https://drafts.css-houdini.org/css-typed-om-1/

use std::collections::BTreeMap;
use std::sync::Arc;

pub use crate::css::calc::{CalcNode, CalcNumericType as NumericType, FfiNumericType};
use crate::css::calc::{CalcNumericValue, base_type_from_value_type, canonical_unit_code, resolve_as_for_value_type};
use crate::css::css_enums::{keyword, keyword_from_ascii_case_insensitive};
use crate::css::css_string::CssString;
use crate::css::dimension_units::{
    ANGLE_UNIT_CANONICAL_RATIOS, ANGLE_UNIT_NAMES, FLEX_UNIT_CANONICAL_RATIOS, FLEX_UNIT_NAMES,
    FREQUENCY_UNIT_CANONICAL_RATIOS, FREQUENCY_UNIT_NAMES, RESOLUTION_UNIT_CANONICAL_RATIOS, RESOLUTION_UNIT_NAMES,
    TIME_UNIT_CANONICAL_RATIOS, TIME_UNIT_NAMES,
};
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::arbitrary_substitution::SubstitutionFunctionsPresence;
use crate::css::parser::arbitrary_substitution::collect_arbitrary_substitution_function_presence;
use crate::css::parser::query_parser::{css_query_evaluate_supports, rust_parse_supports_condition};
use crate::css::parser::syntax::{SyntaxNode, rust_parse_syntax, rust_syntax_is_universal};
use crate::css::parser::value_parser::{
    FfiParseStatus, FfiValueParsingContext, FfiValueParsingContextKind, ParseContext, RandomFunctionIndex,
    component_values_from_source, is_css_wide_keyword, round_to_nearest_integer, rust_parse_css_value,
    rust_parse_entire_css_primitive_from_source,
};
pub use crate::css::property_metadata::property_is_shorthand;
use crate::css::property_metadata::{
    is_custom_property_name, property_accepted_value_types, property_id, property_id_from_name, property_name,
    property_numeric_ranges, property_percentages_resolve_to,
};
use crate::css::serialize::{SerializationMode, TextSink, serialize_style_value};
use crate::css::style_compute::{
    LENGTH_UNIT_CANONICAL_PX_RATIOS, LENGTH_UNIT_NAMES, rust_style_value_is_computationally_independent,
};
use crate::css::style_value::style_value_create_grid_track_size_list;
pub use crate::css::style_value::{ReifiedUnresolvedSegment, RetainedStyleValueData as RetainedStyleValue};
use crate::css::style_value::{
    RetainedNumericRangeByType, StyleValueData, rust_style_value_create_calculated, rust_style_value_create_value_list,
};
use crate::css::style_value_arc::StyleValueArc;
use crate::css::value_codes::{
    VALUE_TYPE_ANGLE, VALUE_TYPE_FLEX, VALUE_TYPE_FREQUENCY, VALUE_TYPE_IMAGE, VALUE_TYPE_INTEGER, VALUE_TYPE_LENGTH,
    VALUE_TYPE_LENGTH_PERCENTAGE, VALUE_TYPE_NUMBER, VALUE_TYPE_OPACITY_VALUE, VALUE_TYPE_PERCENTAGE,
    VALUE_TYPE_RESOLUTION, VALUE_TYPE_TIME, VALUE_TYPE_TRANSFORM_FUNCTION, VALUE_TYPE_TRANSFORM_LIST,
};

/// The base types of a numeric type, as indices of [`NumericType::exponents`].
pub const BASE_TYPE_LENGTH: usize = 0;
pub const BASE_TYPE_ANGLE: usize = 1;
pub const BASE_TYPE_TIME: usize = 2;
pub const BASE_TYPE_FREQUENCY: usize = 3;
pub const BASE_TYPE_RESOLUTION: usize = 4;
pub const BASE_TYPE_FLEX: usize = 5;
pub const BASE_TYPE_PERCENT: usize = 6;

#[derive(Clone, Copy, Eq, PartialEq)]
pub enum DimensionType {
    Angle,
    Flex,
    Frequency,
    Length,
    Resolution,
    Time,
}

impl DimensionType {
    const ALL: [DimensionType; 6] = [
        DimensionType::Angle,
        DimensionType::Flex,
        DimensionType::Frequency,
        DimensionType::Length,
        DimensionType::Resolution,
        DimensionType::Time,
    ];

    fn unit_names(self) -> &'static [&'static str] {
        match self {
            DimensionType::Angle => &ANGLE_UNIT_NAMES,
            DimensionType::Flex => &FLEX_UNIT_NAMES,
            DimensionType::Frequency => &FREQUENCY_UNIT_NAMES,
            DimensionType::Length => &LENGTH_UNIT_NAMES,
            DimensionType::Resolution => &RESOLUTION_UNIT_NAMES,
            DimensionType::Time => &TIME_UNIT_NAMES,
        }
    }

    fn canonical_ratios(self) -> &'static [f64] {
        match self {
            DimensionType::Angle => &ANGLE_UNIT_CANONICAL_RATIOS,
            DimensionType::Flex => &FLEX_UNIT_CANONICAL_RATIOS,
            DimensionType::Frequency => &FREQUENCY_UNIT_CANONICAL_RATIOS,
            DimensionType::Length => &LENGTH_UNIT_CANONICAL_PX_RATIOS,
            DimensionType::Resolution => &RESOLUTION_UNIT_CANONICAL_RATIOS,
            DimensionType::Time => &TIME_UNIT_CANONICAL_RATIOS,
        }
    }

    fn base_type(self) -> usize {
        match self {
            DimensionType::Angle => BASE_TYPE_ANGLE,
            DimensionType::Flex => BASE_TYPE_FLEX,
            DimensionType::Frequency => BASE_TYPE_FREQUENCY,
            DimensionType::Length => BASE_TYPE_LENGTH,
            DimensionType::Resolution => BASE_TYPE_RESOLUTION,
            DimensionType::Time => BASE_TYPE_TIME,
        }
    }

    /// The name of the unit with the code.
    pub fn unit_name(self, unit: u8) -> &'static str {
        self.unit_names()[usize::from(unit)]
    }
}

/// The dimension that the unit belongs to and its code, in any ASCII case.
pub fn dimension_for_unit(unit: &str) -> Option<(DimensionType, u8)> {
    DimensionType::ALL.iter().find_map(|dimension| {
        let code = dimension
            .unit_names()
            .iter()
            .position(|name| name.eq_ignore_ascii_case(unit))?;
        Some((*dimension, u8::try_from(code).expect("a dimension has few units")))
    })
}

/// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-create-a-type
pub fn create_a_type_from_unit(unit: &str) -> Option<NumericType> {
    // To create a type from a string unit, follow the appropriate branch of the following:

    // unit is "number"
    if unit == "number" {
        // Return «[ ]» (empty map)
        return Some(NumericType::default());
    }

    let mut result = NumericType::default();

    // unit is "percent"
    if unit == "percent" {
        // Return «[ "percent" → 1 ]»
        result.exponents[BASE_TYPE_PERCENT] = Some(1);
        return Some(result);
    }

    // unit is a <length> unit, an <angle> unit, a <time> unit, a <frequency> unit, a <resolution-unit> or a <flex> unit
    let (dimension, _) = dimension_for_unit(unit)?;
    result.exponents[dimension.base_type()] = Some(1);
    Some(result)
}

/// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-unit-map
pub type UnitMap = BTreeMap<String, i32>;

/// https://drafts.css-houdini.org/css-typed-om-1/#product-of-two-unit-maps
pub fn product_of_two_unit_maps(units1: &UnitMap, units2: &UnitMap) -> UnitMap {
    // 1. Let result be a copy of units1.
    let mut result = units1.clone();

    // 2. For each unit → power in units2:
    for (unit, power) in units2 {
        // 1. If result[unit] exists, increment result[unit] by power.
        // 2. Otherwise, set result[unit] to power.
        *result.entry(unit.clone()).or_insert(0) += power;
    }

    // 3. Return result.
    result
}

/// https://drafts.css-houdini.org/css-typed-om-1/#create-a-type-from-a-unit-map
pub fn create_a_type_from_a_unit_map(unit_map: &UnitMap) -> Option<NumericType> {
    // 1. Let types be an initially empty list.
    let mut types = Vec::new();

    // 2. For each unit → power in unit map:
    for (unit, power) in unit_map {
        // 1. Let type be the result of creating a type from unit.
        let mut r#type = create_a_type_from_unit(unit).expect("a unit map holds units");

        // 2. Set type’s sole value to power.
        let sole_type = r#type
            .exponents
            .iter()
            .position(Option::is_some)
            .expect("the type of a unit has a value");
        r#type.exponents[sole_type] = Some(*power);

        // 3. Append type to types.
        types.push(r#type);
    }

    // 3. Return the result of multiplying all the items of types.
    // NB: Multiplying no types at all, as for the empty unit map of a plain number, results in an empty type.
    let mut types = types.into_iter();
    let Some(mut result) = types.next() else {
        return Some(NumericType::default());
    };
    for r#type in types {
        result = result.multiplied_by(&r#type)?;
    }
    Some(result)
}

/// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-sum-value
#[derive(Clone)]
pub struct SumValueItem {
    pub value: f64,
    pub unit_map: UnitMap,
}

pub type SumValue = Vec<SumValueItem>;

/// The value in the canonical unit of the set of compatible units that the unit is a member of, with that unit.
pub fn in_canonical_unit(value: f64, unit: &str) -> (f64, String) {
    let Some((dimension, code)) = dimension_for_unit(unit) else {
        return (value, unit.to_owned());
    };
    let ratios = dimension.canonical_ratios();
    let ratio = ratios[usize::from(code)];
    // NB: Relative units have no ratio, and are not members of the set of the canonical unit.
    if ratio.is_nan() {
        return (value, unit.to_owned());
    }
    let canonical_unit = canonical_unit_code(ratios);
    if code == canonical_unit {
        return (value, unit.to_owned());
    }
    (value * ratio, dimension.unit_name(canonical_unit).to_ascii_lowercase())
}

/// The ratio that converts a value in the unit `from` to the unit `to`, if they are compatible.
pub fn conversion_ratio(from: &str, to: &str) -> Option<f64> {
    // NB: If the units are identical, they're always compatible. That also covers cases of `number` and `percent`
    //     which aren't actually units.
    if from == to {
        return Some(1.0);
    }
    let (from_dimension, from_code) = dimension_for_unit(from)?;
    let (to_dimension, to_code) = dimension_for_unit(to)?;
    if from_dimension != to_dimension {
        return None;
    }
    if from_code == to_code {
        return Some(1.0);
    }
    let ratios = from_dimension.canonical_ratios();
    let from_ratio = ratios[usize::from(from_code)];
    let to_ratio = ratios[usize::from(to_code)];
    if from_ratio.is_nan() || to_ratio.is_nan() {
        return None;
    }
    Some(from_ratio / to_ratio)
}

/// A numeric value of a CSSUnitValue as the rest of the engine knows it.
#[derive(Clone, Copy)]
pub enum NumericValue {
    Number(f64),
    Percentage(f64),
    Dimension {
        dimension: DimensionType,
        value: f64,
        unit: u8,
    },
}

impl NumericValue {
    pub fn from_unit(value: f64, unit: &str) -> Option<Self> {
        if unit == "number" {
            return Some(Self::Number(value));
        }
        if unit == "percent" {
            return Some(Self::Percentage(value));
        }
        let (dimension, unit) = dimension_for_unit(unit)?;
        Some(Self::Dimension { dimension, value, unit })
    }

    pub fn calculation_node(self) -> Arc<CalcNode> {
        let numeric = match self {
            Self::Number(value) => CalcNumericValue::Number { value, number_type: 0 },
            Self::Percentage(value) => CalcNumericValue::Percentage(value),
            Self::Dimension { dimension, value, unit } => match dimension {
                DimensionType::Angle => CalcNumericValue::Angle { value, unit },
                DimensionType::Flex => CalcNumericValue::Flex { value, unit },
                DimensionType::Frequency => CalcNumericValue::Frequency { value, unit },
                DimensionType::Length => CalcNumericValue::Length { value, unit },
                DimensionType::Resolution => CalcNumericValue::Resolution { value, unit },
                DimensionType::Time => CalcNumericValue::Time { value, unit },
            },
        };
        Arc::new(CalcNode::Numeric(numeric))
    }

    pub fn style_value(self) -> RetainedStyleValue {
        RetainedStyleValue::from_owned(match self {
            Self::Number(value) => StyleValueData::Number { value },
            Self::Percentage(value) => StyleValueData::Percentage { value },
            Self::Dimension { dimension, value, unit } => match dimension {
                DimensionType::Angle => StyleValueData::Angle { unit, value },
                DimensionType::Flex => StyleValueData::Flex { unit, value },
                DimensionType::Frequency => StyleValueData::Frequency { unit, value },
                DimensionType::Length => StyleValueData::Length { unit, value },
                DimensionType::Resolution => StyleValueData::Resolution { unit, value },
                DimensionType::Time => StyleValueData::Time { unit, value },
            },
        })
    }
}

/// A number serialized with the rules of https://drafts.csswg.org/cssom/#serialize-a-css-value.
pub fn serialize_a_number(s: &mut String, value: f64) {
    let mut sink = TextSink::new();
    crate::css::serialize::serialize_a_number(&mut sink, value, SerializationMode::Normal);
    s.push_str(&String::from_utf16_lossy(&sink.into_utf16()));
}

/// https://drafts.csswg.org/cssom/#serialize-an-identifier
pub fn serialize_an_identifier(identifier: &ak::Utf16String) -> ak::Utf16String {
    let mut sink = TextSink::new();
    let units = identifier.to_utf16();
    crate::css::serialize::serialize_an_identifier(&mut sink, &crate::css::serialize::StringUnits::Utf16(&units));
    ak::Utf16String::from_utf16(&sink.into_utf16())
}

/// https://drafts.csswg.org/css-variables/#custom-property
pub fn is_a_custom_property_name_string(name: &ak::Utf16String) -> bool {
    is_custom_property_name(&name.to_utf16())
}

/// An unresolved value of the text, which keeps the text as it is, or nothing if the arbitrary substitution functions
/// in the text have invalid syntax.
pub fn unresolved_style_value(text: &ak::Utf16String) -> Option<RetainedStyleValue> {
    let units = text.to_utf16();
    let values = component_values_from_source(units.as_ref()).ok()?;
    let mut presence = SubstitutionFunctionsPresence::default();
    collect_arbitrary_substitution_function_presence(&values, &mut presence).ok()?;
    // NB: This is the C++ enumerator UnresolvedStyleValue::SourceTextMode::Preserve.
    const SOURCE_TEXT_MODE_PRESERVE: u8 = 2;
    // SAFETY: The units are readable for their length, and no parsed value is handed over.
    Some(unsafe {
        RetainedStyleValue::from_raw(
            crate::css::style_value::rust_style_value_create_unresolved_from_source(
                core::ptr::null(),
                units.as_ptr(),
                units.len(),
                SOURCE_TEXT_MODE_PRESERVE,
                presence.attr,
                presence.dashed_function,
                presence.env,
                presence.if_,
                presence.inherit,
                presence.var,
                false,
                core::ptr::null(),
            )
            .cast(),
        )
    })
}

impl RetainedStyleValue {
    /// The text of the value, in the numeric mode of the C++ enum SerializationMode.
    pub fn serialize(&self, mode: u8) -> ak::Utf16String {
        let mut sink = TextSink::new();
        serialize_style_value(&mut sink, self.data(), SerializationMode::from_ffi(mode));
        ak::Utf16String::from_utf16(&sink.into_utf16())
    }

    pub fn is_transformation(&self) -> bool {
        matches!(self.data(), StyleValueData::Transformation(_))
    }

    pub fn is_unresolved(&self) -> bool {
        matches!(self.data(), StyleValueData::Unresolved(_))
    }

    pub fn is_custom_ident(&self) -> bool {
        matches!(self.data(), StyleValueData::CustomIdent { .. })
    }

    pub fn is_keyword(&self) -> bool {
        matches!(self.data(), StyleValueData::Keyword { .. })
    }

    pub fn is_css_wide_keyword(&self) -> bool {
        matches!(self.data(), StyleValueData::Keyword { keyword } if is_css_wide_keyword(*keyword))
    }

    pub fn is_shorthand(&self) -> bool {
        matches!(self.data(), StyleValueData::Shorthand(_))
    }

    pub fn is_pending_substitution(&self) -> bool {
        matches!(self.data(), StyleValueData::PendingSubstitution { .. })
    }

    pub fn is_guaranteed_invalid(&self) -> bool {
        matches!(self.data(), StyleValueData::GuaranteedInvalid)
    }

    pub fn is_grid_track_size_list(&self) -> bool {
        matches!(self.data(), StyleValueData::GridTrackSizeList(_))
    }

    pub fn is_length_percentage(&self) -> bool {
        matches!(
            self.data(),
            StyleValueData::Length { .. } | StyleValueData::Percentage { .. } | StyleValueData::Calculated(_)
        )
    }

    /// The items of a value list.
    pub fn value_list_items(&self) -> Option<Vec<RetainedStyleValue>> {
        let StyleValueData::ValueList { values, .. } = self.data() else {
            return None;
        };
        Some(
            values
                .as_slice()
                .iter()
                .map(RetainedStyleValue::clone_retained)
                .collect(),
        )
    }

    /// A text-indent value of the length-percentage, which neither hangs nor applies to each line.
    pub fn into_text_indent(self) -> RetainedStyleValue {
        RetainedStyleValue::from_owned(StyleValueData::TextIndent {
            length_percentage: self,
            hanging: false,
            each_line: false,
        })
    }

    /// A grid track size list of the track sizes.
    pub fn grid_track_size_list(track_sizes: Vec<RetainedStyleValue>) -> RetainedStyleValue {
        let pointers: Vec<_> = track_sizes.into_iter().map(|size| size.into_raw().cast()).collect();
        // SAFETY: This hands over strong references to the track sizes, and takes over the one that the call returns.
        unsafe {
            RetainedStyleValue::from_retained_pointer(style_value_create_grid_track_size_list(
                pointers.as_ptr(),
                pointers.len(),
            ))
        }
    }

    /// Takes over one strong reference, which C++ hands over.
    ///
    /// # Safety
    ///
    /// `pointer` must be a non-null strong reference to a style value.
    pub unsafe fn from_raw(pointer: *const core::ffi::c_void) -> Self {
        // SAFETY: The caller hands over a strong reference.
        unsafe { Self::from_retained_pointer(pointer.cast()) }
    }

    /// Gives up the strong reference, which the caller hands over to C++.
    pub fn into_raw(self) -> *const core::ffi::c_void {
        let pointer = self.pointer();
        core::mem::forget(self);
        pointer.cast()
    }
}

/// What a style value reifies as, https://drafts.css-houdini.org/css-typed-om-1/#reify-stylevalue
pub enum Reification {
    /// A new CSSStyleValue that represents the value.
    Default,
    /// A new CSSImageValue that represents the value.
    Image,
    /// A new CSSKeywordValue with this value.
    Keyword(ak::Utf16String),
    /// A new CSSUnitValue with this value and unit.
    Unit { value: f64, unit: String },
    /// A <number> or <integer>, which is 0px in a property that expects a length.
    Number { value: f64, is_integer: bool },
    /// The reification of a value that this value holds.
    Value(RetainedStyleValue),
    /// The reification of a calculation.
    Calculated,
    /// The reification of a list of component values.
    Unresolved(Vec<ReifiedUnresolvedSegment>),
    /// The reification of a list of values.
    List(Vec<RetainedStyleValue>),
}

fn dimension_reification(dimension: DimensionType, value: f64, unit: u8) -> Reification {
    Reification::Unit {
        value,
        unit: dimension.unit_name(unit).to_owned(),
    }
}

/// The steps of https://drafts.css-houdini.org/css-typed-om-1/#reify-stylevalue for the kind of the value.
pub fn reification_of(value: &RetainedStyleValue) -> Reification {
    match value.data() {
        StyleValueData::ConicGradient(_)
        | StyleValueData::Image(_)
        | StyleValueData::ImageSet(_)
        | StyleValueData::LinearGradient(_)
        | StyleValueData::RadialGradient(_) => return Reification::Image,
        StyleValueData::Angle { unit, value } => return dimension_reification(DimensionType::Angle, *value, *unit),
        StyleValueData::Flex { unit, value } => return dimension_reification(DimensionType::Flex, *value, *unit),
        StyleValueData::Frequency { unit, value } => {
            return dimension_reification(DimensionType::Frequency, *value, *unit);
        }
        StyleValueData::Length { unit, value } => return dimension_reification(DimensionType::Length, *value, *unit),
        StyleValueData::Percentage { value } => {
            return Reification::Unit {
                value: *value,
                unit: "percent".to_owned(),
            };
        }
        StyleValueData::Resolution { unit, value } => {
            return dimension_reification(DimensionType::Resolution, *value, *unit);
        }
        StyleValueData::Time { unit, value } => return dimension_reification(DimensionType::Time, *value, *unit),
        StyleValueData::BackgroundSize(size) => {
            // NB: A background size whose height is `auto` serializes as just its width, so reify it as that width. For
            //     example, the `auto` keyword is parsed as a size of `auto auto`.
            if matches!(size.size_y.data(), StyleValueData::Keyword { keyword } if *keyword == keyword::AUTO) {
                return Reification::Value(size.size_x.clone());
            }
        }
        StyleValueData::Calculated(_) => return Reification::Calculated,
        StyleValueData::CustomIdent { custom_ident } => {
            // https://drafts.css-houdini.org/css-typed-om-1/#reify-ident
            // 1. Return a new CSSKeywordValue with its value internal slot set to the serialization of ident.
            return Reification::Keyword(ak::Utf16String::from_utf16(custom_ident.units()));
        }
        StyleValueData::Easing(easing) => {
            // NB: Preserve authored step keywords until computation canonicalizes them to steps().
            if easing.is_keyword {
                // NB: This is the C++ enumerator StepPosition::Start.
                const STEP_POSITION_START: u8 = 4;
                let keyword = if easing.step_position == STEP_POSITION_START {
                    "step-start"
                } else {
                    "step-end"
                };
                return Reification::Keyword(ak::Utf16String::from_utf8(keyword));
            }
        }
        StyleValueData::Integer { value } => {
            return Reification::Number {
                value: f64::from(*value),
                is_integer: true,
            };
        }
        StyleValueData::Keyword { .. } => {
            // https://drafts.css-houdini.org/css-typed-om-1/#reify-ident
            // 1. Return a new CSSKeywordValue with its value internal slot set to the serialization of ident.
            return Reification::Keyword(value.serialize(0));
        }
        StyleValueData::Number { value } => {
            return Reification::Number {
                value: *value,
                is_integer: false,
            };
        }
        StyleValueData::OpacityValue { value } => return Reification::Value(value.clone()),
        StyleValueData::GridTrackSizeList(_) => {
            // NB: A track list that is just one track size, as for `grid-auto-columns: 10px`, is that track size.
            let track_size = crate::css::style_value::style_value_grid_track_size_list_single_size(value.data());
            if !track_size.is_null() {
                // SAFETY: The value holds the track size alive, and this takes its own strong reference.
                return Reification::Value(unsafe {
                    RetainedStyleValue::from_raw(crate::css::style_value::rust_style_value_retain(track_size).cast())
                });
            }
        }
        StyleValueData::TextIndent {
            hanging,
            each_line,
            length_percentage,
        } => {
            // NB: A text-indent without the `hanging` or `each-line` keywords is just its length-percentage.
            if !hanging && !each_line {
                return Reification::Value(length_percentage.clone());
            }
        }
        StyleValueData::Unresolved(_) => {
            return Reification::Unresolved(crate::css::style_value::reified_unresolved_segments(value.data()));
        }
        StyleValueData::ValueList { values, .. } => {
            return Reification::List(values.as_slice().to_vec());
        }
        _ => {}
    }

    // AD-HOC: Some values that are just a single keyword are represented by a more specific type, for example
    //         `font-style: italic`, `grid-auto-flow: row` or `font-variant-numeric: ordinal`. Reify those as the
    //         keyword. Colors and shorthands are left alone, as they are not keywords on their own.
    if !matches!(
        value.data(),
        StyleValueData::ColorFunction(_)
            | StyleValueData::ColorMix(_)
            | StyleValueData::ContrastColor { .. }
            | StyleValueData::Shorthand(_)
    ) {
        let serialized_value = value.serialize(0);
        if keyword_from_ascii_case_insensitive(&serialized_value.to_utf16()).is_some() {
            return Reification::Keyword(serialized_value);
        }
    }

    Reification::Default
}

/// The nodes of a calculation in postorder, and the indices of their children, if the calculation has no math function
/// that is not reifiable yet.
pub fn calculation_nodes(value: &RetainedStyleValue) -> Option<(Vec<CalcReificationNode>, Vec<usize>)> {
    crate::css::calc::reification_nodes(value.data())
}

pub use crate::css::calc::CalcReificationNode;

/// https://drafts.csswg.org/css-transforms-2/#typedef-transform-function
#[derive(Clone, Copy, Eq, PartialEq)]
#[repr(u8)]
pub enum TransformFunction {
    Matrix,
    Matrix3d,
    Perspective,
    Translate,
    Translate3d,
    TranslateX,
    TranslateY,
    TranslateZ,
    Scale,
    Scale3d,
    ScaleX,
    ScaleY,
    ScaleZ,
    Rotate,
    Rotate3d,
    RotateX,
    RotateY,
    RotateZ,
    Skew,
    SkewX,
    SkewY,
}

impl TransformFunction {
    /// The functions, in the order of the codes that TransformFunctions.json gives them.
    const ALL: [Self; 21] = [
        Self::Matrix,
        Self::Matrix3d,
        Self::Perspective,
        Self::Translate,
        Self::Translate3d,
        Self::TranslateX,
        Self::TranslateY,
        Self::TranslateZ,
        Self::Scale,
        Self::Scale3d,
        Self::ScaleX,
        Self::ScaleY,
        Self::ScaleZ,
        Self::Rotate,
        Self::Rotate3d,
        Self::RotateX,
        Self::RotateY,
        Self::RotateZ,
        Self::Skew,
        Self::SkewX,
        Self::SkewY,
    ];
}

/// A <transform-function> value of the property.
pub fn transformation_style_value(
    property_id: u16,
    transform_function: TransformFunction,
    values: Vec<RetainedStyleValue>,
) -> RetainedStyleValue {
    let values: Vec<_> = values.into_iter().map(|value| value.into_raw().cast()).collect();
    // SAFETY: This hands over one strong reference to each of the values.
    unsafe {
        RetainedStyleValue::from_raw(
            crate::css::style_value::rust_style_value_create_transformation(
                property_id,
                transform_function as u8,
                values.as_ptr(),
                values.len(),
            )
            .cast(),
        )
    }
}

/// The function and the arguments of a <transform-function> value.
pub fn transformation_parts(value: &RetainedStyleValue) -> (TransformFunction, Vec<RetainedStyleValue>) {
    let StyleValueData::Transformation(transformation) = value.data() else {
        panic!("the value is a transform function");
    };
    (
        TransformFunction::ALL[usize::from(transformation.transform_function)],
        transformation.values.as_slice().to_vec(),
    )
}

/// The matrix of a <transform-function> value, row by row, if it has one without a layout.
pub fn transform_matrix(value: &RetainedStyleValue) -> Option<[f32; 16]> {
    let mut matrix = [0.0_f32; 16];
    let mut is_2d = true;
    crate::css::table_group_builder::rust_transform_list_to_abstract_matrix(
        value.data(),
        ak::SpanMut::new(matrix.as_mut_slice()),
        &mut is_2d,
    )
    .then_some(matrix)
}

/// The value types of a property that the CSS Typed OM asks about by name.
#[derive(Clone, Copy)]
pub enum TypedOmValueType {
    Image,
    TransformFunction,
    TransformList,
}

/// Whether a value that is not a shorthand's can be of the value type. A custom property takes no fixed types.
fn property_accepts_value_type(property_id: u16, value_type: u8) -> bool {
    property_id != property_id::CUSTOM && property_accepted_value_types(property_id).contains(&value_type)
}

pub fn property_accepts_type(property_id: u16, value_type: TypedOmValueType) -> bool {
    property_accepts_value_type(
        property_id,
        match value_type {
            TypedOmValueType::Image => VALUE_TYPE_IMAGE,
            TypedOmValueType::TransformFunction => VALUE_TYPE_TRANSFORM_FUNCTION,
            TypedOmValueType::TransformList => VALUE_TYPE_TRANSFORM_LIST,
        },
    )
}

/// Whether the value is in the range of the value type of the property. A bound that the tables write as the limit of
/// a float stands for no bound at all.
fn property_accepts_value_in_range(property_id: u16, value_type: u8, value: f64) -> bool {
    if !property_accepts_value_type(property_id, value_type) {
        return false;
    }
    property_numeric_ranges(property_id)
        .iter()
        .find(|range| range.value_type == value_type)
        .is_none_or(|range| {
            (range.min == f32::MIN as f64 || value >= range.min) && (range.max == f32::MAX as f64 || value <= range.max)
        })
}

fn percentages_resolve_as(property_id: u16) -> Option<u8> {
    if property_id == property_id::CUSTOM {
        return None;
    }
    property_percentages_resolve_to(property_id)
}

/// A calculation with the context of the property, https://drafts.csswg.org/css-values-4/#calc-type-checking
fn calculated_value(property_id: u16, calculation: Arc<CalcNode>, numeric_type: NumericType) -> RetainedStyleValue {
    let resolve_as = percentages_resolve_as(property_id);
    let ranges: Vec<_> = if property_id == property_id::CUSTOM {
        Vec::new()
    } else {
        property_numeric_ranges(property_id)
            .iter()
            .map(|range| RetainedNumericRangeByType::new(range.value_type, range.min, range.max))
            .collect()
    };
    let calculation = Arc::into_raw(calculation);
    // SAFETY: This hands over one strong reference to the calculation, and the ranges are readable for their length.
    let value = unsafe {
        rust_style_value_create_calculated(
            calculation,
            FfiNumericType::from_calc(Some(numeric_type)),
            resolve_as.is_some(),
            resolve_as == Some(VALUE_TYPE_NUMBER),
            resolve_as.and_then(base_type_from_value_type).unwrap_or(0),
            resolve_as.unwrap_or(0),
            property_accepts_value_type(property_id, VALUE_TYPE_INTEGER),
            ranges.as_ptr(),
            ranges.len(),
        )
    };
    // SAFETY: The call hands over a strong reference to a new value.
    unsafe { RetainedStyleValue::from_raw(value.cast()) }
}

fn opacity_value(value: RetainedStyleValue) -> RetainedStyleValue {
    RetainedStyleValue::from_arc(StyleValueArc::new(StyleValueData::OpacityValue { value }))
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
pub fn numeric_style_value(
    property_id: u16,
    value: RetainedStyleValue,
    calculation: Arc<CalcNode>,
    numeric_type: NumericType,
) -> Option<RetainedStyleValue> {
    // If any component of property’s CSS grammar has a limited numeric range, and the corresponding part of value is a
    // CSSUnitValue that is outside of that range, replace that value with the result of wrapping it in a fresh
    // CSSMathSum whose values internal slot contains only that part of value.
    let wrap_in_math_sum = || calculated_value(property_id, calculation.clone(), numeric_type);
    let accepted = |value_type: u8, number: f64| property_accepts_value_in_range(property_id, value_type, number);
    let in_range_or_wrapped = |value_type: u8, number: f64, value: RetainedStyleValue| {
        if !property_accepts_value_type(property_id, value_type) {
            return None;
        }
        Some(if accepted(value_type, number) {
            value
        } else {
            wrap_in_math_sum()
        })
    };

    match *value.data() {
        StyleValueData::Number { value: number } => {
            // NB: Our parser represents an <opacity-value> as an opacity value that holds the number.
            if property_accepts_value_type(property_id, VALUE_TYPE_OPACITY_VALUE) {
                return Some(opacity_value(value));
            }

            // NB: Prefer <number> to <integer> when both are accepted.
            if property_accepts_value_type(property_id, VALUE_TYPE_NUMBER) {
                return in_range_or_wrapped(VALUE_TYPE_NUMBER, number, value);
            }

            if property_accepts_value_type(property_id, VALUE_TYPE_INTEGER) {
                let integer = round_to_nearest_integer(number);
                // NB: A number that is not an integer does not match <integer>, so it's wrapped in a CSSMathSum, like a
                //     value outside of the accepted range. It then gets rounded when computed.
                if f64::from(integer) != number || !accepted(VALUE_TYPE_INTEGER, f64::from(integer)) {
                    return Some(wrap_in_math_sum());
                }
                return Some(RetainedStyleValue::from_arc(StyleValueArc::new(
                    StyleValueData::Integer { value: integer },
                )));
            }
            None
        }
        StyleValueData::Percentage { value: percentage } => {
            // NB: Our parser represents an <opacity-value> as an opacity value that holds the percentage.
            if property_accepts_value_type(property_id, VALUE_TYPE_OPACITY_VALUE) {
                return Some(opacity_value(value));
            }
            in_range_or_wrapped(VALUE_TYPE_PERCENTAGE, percentage, value)
        }
        // NB: Out-of-range flex values cannot be deferred in a calculation.
        StyleValueData::Flex { value: flex, .. } => accepted(VALUE_TYPE_FLEX, flex).then_some(value),
        StyleValueData::Angle { value: angle, .. } => in_range_or_wrapped(VALUE_TYPE_ANGLE, angle, value),
        StyleValueData::Frequency { value: frequency, .. } => {
            in_range_or_wrapped(VALUE_TYPE_FREQUENCY, frequency, value)
        }
        StyleValueData::Length { value: length, .. } => in_range_or_wrapped(VALUE_TYPE_LENGTH, length, value),
        StyleValueData::Resolution { value: resolution, .. } => {
            in_range_or_wrapped(VALUE_TYPE_RESOLUTION, resolution, value)
        }
        StyleValueData::Time { value: time, .. } => in_range_or_wrapped(VALUE_TYPE_TIME, time, value),
        _ => unreachable!("a numeric value is one of the numeric style values"),
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
pub fn calculated_style_value(
    property_id: u16,
    calculation: Arc<CalcNode>,
    numeric_type: NumericType,
    perform_type_check: bool,
) -> Option<RetainedStyleValue> {
    let resolve_as = resolve_as_for_value_type(percentages_resolve_as(property_id));
    let accepts = |value_type: u8| property_accepts_value_type(property_id, value_type);

    let matches = if numeric_type.matches_dimension(BASE_TYPE_ANGLE, resolve_as) {
        accepts(VALUE_TYPE_ANGLE)
    } else if numeric_type.matches_dimension(BASE_TYPE_FLEX, resolve_as) {
        // https://drafts.csswg.org/css-grid-2/#fr-unit
        // NB: <flex> values "cannot be represented in or combined with other unit types in calc() expressions".
        false
    } else if numeric_type.matches_dimension(BASE_TYPE_FREQUENCY, resolve_as) {
        accepts(VALUE_TYPE_FREQUENCY)
    } else if numeric_type.matches_dimension(BASE_TYPE_LENGTH, resolve_as) {
        accepts(VALUE_TYPE_LENGTH)
    } else if numeric_type.matches_number(resolve_as) {
        accepts(VALUE_TYPE_NUMBER) || accepts(VALUE_TYPE_INTEGER) || accepts(VALUE_TYPE_OPACITY_VALUE)
    } else if numeric_type.matches_percentage() {
        accepts(VALUE_TYPE_PERCENTAGE) || accepts(VALUE_TYPE_OPACITY_VALUE)
    } else if numeric_type.matches_dimension(BASE_TYPE_RESOLUTION, resolve_as) {
        accepts(VALUE_TYPE_RESOLUTION)
    } else if numeric_type.matches_dimension(BASE_TYPE_TIME, resolve_as) {
        accepts(VALUE_TYPE_TIME)
    } else {
        false
    };
    if perform_type_check && !matches {
        return None;
    }

    let is_number_or_percentage = numeric_type.matches_number(None) || numeric_type.matches_percentage();
    let calculated = calculated_value(property_id, calculation, numeric_type);

    // NB: Our parser represents an <opacity-value> as an opacity value that holds the calculation.
    if accepts(VALUE_TYPE_OPACITY_VALUE) && is_number_or_percentage {
        return Some(opacity_value(calculated));
    }
    Some(calculated)
}

/// The value of the text for the property, with no document to parse it for.
pub fn parse_value_for_property(property_id: u16, css_text: &ak::Utf16String) -> Option<RetainedStyleValue> {
    if css_text.is_empty() {
        return None;
    }
    let value_context = FfiValueParsingContext {
        kind: FfiValueParsingContextKind::Property,
        value: property_id,
        secondary_value: 0,
        name: FfiUtf16View::default(),
    };
    let random_function_index = RandomFunctionIndex::new(0);
    let context = ParseContext {
        value_contexts: ak::Span::new(core::slice::from_ref(&value_context)),
        random_function_index: Some(&random_function_index),
        ..Default::default()
    };
    let mut status = FfiParseStatus::NotHandled;
    // SAFETY: The context and the text are live through the call.
    let value = unsafe {
        rust_parse_css_value(
            Some(&context),
            property_id,
            FfiUtf16View::from_input(css_text.as_units().into()),
            Some(&mut status),
        )
    };
    // SAFETY: A parsed value comes with a strong reference.
    (!value.is_null()).then(|| unsafe { RetainedStyleValue::from_raw(value) })
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
pub fn keyword_style_value(
    property_id: u16,
    value: &ak::Utf16String,
    perform_type_check: bool,
    property_is_list_valued: bool,
) -> Option<RetainedStyleValue> {
    let units = value.to_utf16();
    let keyword = keyword_from_ascii_case_insensitive(units.as_ref());
    let is_css_wide = keyword.is_some_and(is_css_wide_keyword);

    // https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue-match-a-grammar
    // NB: The keywords listed for a property are not all valid on their own, like `hanging` for text-indent, and a
    //     property may accept identifiers that are not listed, like a <custom-ident> or the axis keywords of
    //     scroll-snap-type. So we check if the property accepts the value, as an identifier, when parsed.
    if perform_type_check
        && !is_css_wide
        && parse_value_for_property(property_id, &serialize_an_identifier(value)).is_none()
    {
        return None;
    }

    let Some(keyword) = keyword else {
        return Some(RetainedStyleValue::from_arc(StyleValueArc::new(
            StyleValueData::CustomIdent {
                custom_ident: CssString::from_utf16(units.as_ref()),
            },
        )));
    };

    // NB: Style computation expects the same internal representation that the CSS parser produces for the
    //     property. For example, `display` keywords become a DisplayStyleValue, and keyword combinations like
    //     `text-decoration-line` or `white-space-trim` become a list even when only one keyword is given. So we
    //     parse the keyword as a value of the property to get that representation. List-valued properties are
    //     instead wrapped in a list by StylePropertyMap.
    if !is_css_wide
        && property_id != property_id::CUSTOM
        && !property_is_list_valued
        && let Some(parsed_value) = parse_value_for_property(property_id, value)
    {
        return Some(parsed_value);
    }
    Some(RetainedStyleValue::from_arc(StyleValueArc::new(
        StyleValueData::Keyword { keyword },
    )))
}

/// A list of the values, separated by commas or spaces.
pub fn style_value_list(values: Vec<RetainedStyleValue>, comma: bool) -> RetainedStyleValue {
    let values: Vec<_> = values.into_iter().map(RetainedStyleValue::into_raw).collect();
    // NB: These are the C++ enumerators StyleValueList::Separator::Space and Comma.
    let separator = u8::from(comma);
    // SAFETY: This hands over one strong reference to each of the values.
    let list = unsafe { rust_style_value_create_value_list(values.as_ptr().cast(), values.len(), separator, false) };
    // SAFETY: The call hands over a strong reference to a new value.
    unsafe { RetainedStyleValue::from_raw(list.cast()) }
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-parse
pub fn parse_a_numeric_value(css_text: &ak::Utf16String) -> Option<RetainedStyleValue> {
    if css_text.is_empty() {
        return None;
    }
    let random_function_index = RandomFunctionIndex::new(0);
    let context = ParseContext {
        random_function_index: Some(&random_function_index),
        ..Default::default()
    };
    [
        VALUE_TYPE_NUMBER,
        VALUE_TYPE_PERCENTAGE,
        VALUE_TYPE_LENGTH,
        VALUE_TYPE_LENGTH_PERCENTAGE,
        VALUE_TYPE_ANGLE,
        VALUE_TYPE_TIME,
        VALUE_TYPE_FREQUENCY,
        VALUE_TYPE_RESOLUTION,
        VALUE_TYPE_FLEX,
    ]
    .into_iter()
    .find_map(|value_type| {
        // SAFETY: The context and the text are live through the call.
        let value = unsafe {
            rust_parse_entire_css_primitive_from_source(
                Some(&context),
                value_type,
                FfiUtf16View::from_input(css_text.as_units().into()),
                f32::MIN as f64,
                f32::MAX as f64,
            )
        };
        // SAFETY: A parsed value comes with a strong reference.
        (!value.is_null()).then(|| unsafe { RetainedStyleValue::from_raw(value) })
    })
}

/// The canonical name and the id of the property with the name, if there is one.
pub fn property_from_name(name: &ak::Utf16String) -> Option<(ak::Utf16String, u16)> {
    let units = name.to_utf16();
    let property_id = property_id_from_name(units.as_ref())?;
    if property_id == property_id::CUSTOM {
        return Some((name.clone(), property_id));
    }
    Some((ak::Utf16String::from_utf8(property_name(property_id)), property_id))
}

/// Whether the property expects a 0 to be a <length>.
pub fn zero_is_a_length(property: &ak::Utf16String, is_integer: bool) -> bool {
    let units = property.to_utf16();
    let Some(property_id) = property_id_from_name(units.as_ref()) else {
        return false;
    };
    if !property_accepts_value_type(property_id, VALUE_TYPE_LENGTH) {
        return false;
    }
    is_integer
        || (!property_accepts_value_type(property_id, VALUE_TYPE_NUMBER)
            && !property_accepts_value_type(property_id, VALUE_TYPE_INTEGER))
}

/// A syntax definition, https://drafts.css-houdini.org/css-properties-values-api/#syntax-strings
pub struct RegisteredSyntax(*const core::ffi::c_void);

impl RegisteredSyntax {
    /// https://drafts.css-houdini.org/css-properties-values-api/#consume-syntax-definition
    pub fn parse(syntax: &ak::Utf16String) -> Option<Self> {
        let parsed = rust_parse_syntax(FfiUtf16View::from_input(syntax.as_units().into()), true);
        (!parsed.is_null()).then(|| Self(parsed))
    }

    pub fn is_universal(&self) -> bool {
        // SAFETY: The syntax tree is live.
        rust_syntax_is_universal(unsafe { &*self.0.cast() })
    }

    pub fn as_ptr(&self) -> *const core::ffi::c_void {
        self.0
    }

    /// Gives up the strong reference, which the caller hands over to C++.
    pub fn into_raw(self) -> *const core::ffi::c_void {
        let pointer = self.0;
        core::mem::forget(self);
        pointer
    }
}

impl Drop for RegisteredSyntax {
    fn drop(&mut self) {
        // SAFETY: This owns one strong reference to the syntax tree.
        drop(unsafe { Arc::from_raw(self.0.cast::<SyntaxNode>()) });
    }
}

impl RetainedStyleValue {
    /// https://drafts.css-houdini.org/css-properties-values-api/#computationally-independent
    pub fn is_computationally_independent(&self) -> bool {
        rust_style_value_is_computationally_independent(self.data())
    }
}

/// The non-generic font families of a value of the font property, or None if it does not parse as one.
pub fn font_family_names(font: &ak::Utf16String) -> Option<Vec<ak::Utf16String>> {
    let value = parse_value_for_property(property_id::FONT, font)?;
    let StyleValueData::Shorthand(shorthand) = value.data() else {
        return None;
    };
    let index = shorthand
        .sub_properties
        .as_slice()
        .iter()
        .position(|&property| property == property_id::FONT_FAMILY)?;
    let StyleValueData::ValueList { values, .. } = shorthand.values.as_slice()[index].data() else {
        unreachable!("the font-family of a font shorthand is a list");
    };
    Some(
        values
            .as_slice()
            .iter()
            .filter_map(|family| match family.data() {
                StyleValueData::Keyword { .. } => None,
                StyleValueData::String { string, .. } => Some(ak::Utf16String::from_utf16(string.units())),
                StyleValueData::CustomIdent { custom_ident } => Some(ak::Utf16String::from_utf16(custom_ident.units())),
                _ => unreachable!("a font family is a keyword, a string or a custom identifier"),
            })
            .collect(),
    )
}

/// Whether the text matches as a <supports-condition>, with no document to parse it for.
pub fn supports_condition_matches(condition_text: &ak::Utf16String) -> bool {
    if condition_text.is_empty() {
        return false;
    }
    let random_function_index = RandomFunctionIndex::new(0);
    let context = ParseContext {
        random_function_index: Some(&random_function_index),
        ..Default::default()
    };
    // SAFETY: The context and the text are live through the call.
    let handle = unsafe {
        rust_parse_supports_condition(
            FfiUtf16View::from_input(condition_text.as_units().into()),
            Some(&context),
        )
    };
    if handle.is_null() {
        return false;
    }
    // SAFETY: Parsing handed over a strong reference to a live handle.
    let handle = unsafe { Arc::from_raw(handle) };
    // NB: This is the C++ enumerator MatchResult::True.
    const MATCH_RESULT_TRUE: u8 = 1;
    css_query_evaluate_supports(Some(&handle)) == MATCH_RESULT_TRUE
}
