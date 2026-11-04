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
use crate::css::calc::{CalcNumericValue, canonical_unit_code};
use crate::css::css_enums::{keyword, keyword_from_ascii_case_insensitive};
use crate::css::dimension_units::{
    ANGLE_UNIT_CANONICAL_RATIOS, ANGLE_UNIT_NAMES, FLEX_UNIT_CANONICAL_RATIOS, FLEX_UNIT_NAMES,
    FREQUENCY_UNIT_CANONICAL_RATIOS, FREQUENCY_UNIT_NAMES, RESOLUTION_UNIT_CANONICAL_RATIOS, RESOLUTION_UNIT_NAMES,
    TIME_UNIT_CANONICAL_RATIOS, TIME_UNIT_NAMES,
};
use crate::css::parser::arbitrary_substitution::SubstitutionFunctionsPresence;
use crate::css::parser::arbitrary_substitution::collect_arbitrary_substitution_function_presence;
use crate::css::parser::value_parser::component_values_from_source;
use crate::css::property_metadata::is_custom_property_name;
use crate::css::serialize::{SerializationMode, TextSink, serialize_style_value};
use crate::css::style_compute::{LENGTH_UNIT_CANONICAL_PX_RATIOS, LENGTH_UNIT_NAMES};
use crate::css::style_value::StyleValueData;
pub use crate::css::style_value::{ReifiedUnresolvedSegment, RetainedStyleValueData as RetainedStyleValue};

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
    (value * ratio, dimension.unit_name(canonical_unit).to_owned())
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
            let track_size = crate::css::style_value::rust_style_value_grid_track_size_list_single_size(value.data());
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
