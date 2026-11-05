/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the interfaces of the CSS Typed OM share: reification, and how values reach the property tables and the parser.
//! https://drafts.css-houdini.org/css-typed-om-1/

use core::ffi::c_void;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{MarkedVec, Vm};
use libweb_rust::typed_om::{
    self, CalcNode, CalcReificationNode, DimensionType, NumericType, NumericValue, Reification,
    ReifiedUnresolvedSegment, RetainedStyleValue, calculation_nodes, reification_of,
};
pub(crate) use libweb_rust::typed_om::{TypedOmValueType, property_accepts_type};

use super::css_image_value::CSSImageValue;
use super::css_keyword_value::CSSKeywordValue;
use super::css_math_clamp::CSSMathClamp;
use super::css_math_invert::CSSMathInvert;
use super::css_math_max::CSSMathMax;
use super::css_math_min::CSSMathMin;
use super::css_math_negate::CSSMathNegate;
use super::css_math_product::CSSMathProduct;
use super::css_math_sum::CSSMathSum;
use super::css_numeric_array::CSSNumericArray;
use super::css_numeric_value::AnyCSSNumericValue;
use super::css_style_value::{AnyCSSStyleValue, CSSStyleValue, PerformTypeCheck};
use super::css_transform_component::{AnyCSSTransformComponent, reify_a_transform_function};
use super::css_transform_value::CSSTransformValue;
use super::css_unit_value::CSSUnitValue;
use super::css_unparsed_value::{CSSUnparsedSegment, CSSUnparsedValue};
use super::css_variable_reference_value::CSSVariableReferenceValue;
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::Ref;
use crate::bindings::{CallContext, Exception, WebIdlResult};

/// The string as a Rust string, for what is ASCII or only goes into a message.
pub(crate) fn to_rust_string(string: &Utf16String) -> String {
    String::from_utf16_lossy(&string.to_utf16())
}

unsafe extern "C" {
    fn web_css_style_map_property_is_list_valued(property_id: u16) -> bool;
}

pub(crate) fn property_is_list_valued(property_id: u16) -> bool {
    // SAFETY: The call has no preconditions.
    unsafe { web_css_style_map_property_is_list_valued(property_id) }
}

/// https://drafts.css-houdini.org/css-typed-om-1/#subdivide-into-iterations
fn subdivide_into_iterations(value: RetainedStyleValue, property_id: u16) -> Vec<RetainedStyleValue> {
    // 1. If property is a single-valued property, return a list containing whole value.
    // 2. Otherwise, divide whole value into individual iterations, as appropriate for property, and return a list
    //    containing the iterations in order.
    if property_is_list_valued(property_id)
        && let Some(iterations) = value.value_list_items()
    {
        return iterations;
    }
    vec![value]
}

pub(crate) fn property_accepts_transform_function(property_id: u16) -> bool {
    property_accepts_type(property_id, TypedOmValueType::TransformFunction)
}

pub(crate) fn property_accepts_transform_list(property_id: u16) -> bool {
    property_accepts_type(property_id, TypedOmValueType::TransformList)
}

/// The value of the property for a CSSUnitValue, which is wrapped in a calculation if the property does not accept it
/// on its own, if the property takes values of its type.
pub(crate) fn numeric_style_value_for_property(
    property_id: u16,
    value: NumericValue,
    numeric_type: NumericType,
) -> Option<RetainedStyleValue> {
    let calculation = Arc::new(CalcNode::Sum(vec![value.calculation_node()]));
    typed_om::numeric_style_value(property_id, value.style_value(), calculation, numeric_type)
}

/// The calculated value of the property for a CSSMathValue, if the property takes values of its type.
pub(crate) fn calculated_style_value(
    property_id: u16,
    calculation: Arc<CalcNode>,
    numeric_type: NumericType,
    perform_type_check: PerformTypeCheck,
) -> Option<RetainedStyleValue> {
    typed_om::calculated_style_value(
        property_id,
        calculation,
        numeric_type,
        perform_type_check == PerformTypeCheck::Yes,
    )
}

/// The value of the property for a CSSKeywordValue, if the keyword matches the grammar of the property.
pub(crate) fn keyword_style_value_for_property(
    property_id: u16,
    value: &Utf16String,
    perform_type_check: PerformTypeCheck,
) -> Option<RetainedStyleValue> {
    typed_om::keyword_style_value(
        property_id,
        value,
        perform_type_check == PerformTypeCheck::Yes,
        property_is_list_valued(property_id),
    )
}

pub(crate) fn style_value_list(values: Vec<RetainedStyleValue>) -> RetainedStyleValue {
    typed_om::style_value_list(values, false)
}

pub(crate) fn comma_separated_style_value_list(values: Vec<RetainedStyleValue>) -> RetainedStyleValue {
    typed_om::style_value_list(values, true)
}

pub(crate) unsafe extern "C" fn append_style_value(values: *mut c_void, value: *const c_void) {
    // SAFETY: The context is the Vec of parse_a_css_style_value(), and C++ hands over a strong reference.
    unsafe { (*values.cast::<Vec<RetainedStyleValue>>()).push(RetainedStyleValue::from_raw(value)) };
}

// https://drafts.css-houdini.org/css-typed-om-1/#parse-a-cssstylevalue
pub(crate) fn parse_values(
    vm: &Vm,
    property: Utf16String,
    css_text: Utf16String,
) -> WebIdlResult<MarkedVec<'_, AnyCSSStyleValue>> {
    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    // 2. If property is not a valid CSS property, throw a TypeError.
    let Some((name, property_id)) = typed_om::property_from_name(&property) else {
        return Err(Exception::TypeError(Utf16String::from_utf8(&format!(
            "'{}' is not a valid CSS property",
            to_rust_string(&property)
        ))));
    };

    // 3. Attempt to parse cssText according to property’s grammar.
    //    If this fails, throw a TypeError.
    //    Otherwise, let whole value be the parsed result.
    let Some(whole_value) = typed_om::parse_value_for_property(property_id, &css_text) else {
        return Err(Exception::TypeError(Utf16String::from_utf8(&format!(
            "Failed to parse '{}' as a value for '{}' property",
            to_rust_string(&css_text),
            to_rust_string(&property)
        ))));
    };

    // 4. Subdivide into iterations whole value, according to property, and let values be the result.
    let values = subdivide_into_iterations(whole_value, property_id);

    // 5. For each value in values, replace it with the result of reifying value for property.
    let reified_values = MarkedVec::new(vm);
    for value in &values {
        reified_values.push(reify(vm, value, &name));
    }

    // 6. If parseMultiple is false, return values[0]. Otherwise, return values.
    // FIXME: We need to somehow store the source css_text on the returned CSSStyleValue.
    //        https://github.com/w3c/css-houdini-drafts/issues/1156
    Ok(reified_values)
}

pub(crate) fn parse_a_css_style_value<'vm>(
    cx: &mut CallContext<'vm>,
    property: UsvString,
    css_text: UsvString,
) -> WebIdlResult<MarkedVec<'vm, AnyCSSStyleValue>> {
    parse_values(cx.vm(), property.into_utf16_string(), css_text.into_utf16_string())
}

fn unit_value(vm: &Vm, value: f64, unit: &str) -> AnyCSSStyleValue {
    AnyCSSNumericValue::CSSUnitValue(CSSUnitValue::create(vm, value, unit)).into()
}

fn default_reification(vm: &Vm, value: &RetainedStyleValue, property: &Utf16String) -> AnyCSSStyleValue {
    // 1. Return a new CSSStyleValue object representing value whose [[associatedProperty]] internal slot is set to
    //    property.
    AnyCSSStyleValue::CSSStyleValue(CSSStyleValue::create(vm, property.clone(), value.clone(), 0))
}

/// https://drafts.css-houdini.org/css-typed-om-1/#reify-stylevalue
pub(crate) fn reify(vm: &Vm, value: &RetainedStyleValue, property: &Utf16String) -> AnyCSSStyleValue {
    match reification_of(value) {
        Reification::Default => default_reification(vm, value, property),
        // AD-HOC: There's no spec description of how to reify as a CSSImageValue.
        Reification::Image => AnyCSSStyleValue::CSSImageValue(CSSImageValue::create(vm, value.clone())),
        // https://drafts.css-houdini.org/css-typed-om-1/#reify-ident
        Reification::Keyword(keyword) => AnyCSSStyleValue::CSSKeywordValue(CSSKeywordValue::create(vm, keyword)),
        // https://drafts.css-houdini.org/css-typed-om-1/#reify-a-numeric-value
        // NB: Steps 1 and 2 don't apply here.
        // 3. Return a new CSSUnitValue with its value internal slot set to the numeric value of num, and its unit
        //    internal slot set to "number" if num is a <number>, "percent" if num is a <percentage>, and num’s unit if
        //    num is a <dimension>.
        //    If the value being reified is a computed value, the unit used must be the appropriate canonical unit for
        //    the value’s type, with the numeric value scaled accordingly.
        // FIXME: Reify computed value correctly. That sounds like it should work by computing the value properly before
        //        we reach this point.
        Reification::Unit { value, unit } => unit_value(vm, value, &unit),
        // NB: Step 1 doesn't apply here.
        // 2. If num is the unitless value 0 and num is a <dimension>, return a new CSSUnitValue with its value internal
        //    slot set to 0, and its unit internal slot set to "px".
        Reification::Number { value, is_integer } => {
            // NB: Determine whether the associated property expects 0 to be a <length>.
            // FIXME: Do this for registered custom properties.
            if value == 0.0 && typed_om::zero_is_a_length(property, is_integer) {
                return unit_value(vm, 0.0, "px");
            }
            unit_value(vm, value, "number")
        }
        Reification::Value(inner_value) => reify(vm, &inner_value, property),
        // NB: This spec algorithm is incomplete and assumes we do not already have a calculation tree.
        Reification::Calculated => reify_a_math_expression(vm, value)
            .map(AnyCSSStyleValue::from)
            // Some math functions are not reifiable yet. If we contain one, we have to fall back to CSSStyleValue.
            // https://github.com/w3c/css-houdini-drafts/issues/1090
            .unwrap_or_else(|| default_reification(vm, value, property)),
        Reification::Unresolved(segments) => {
            AnyCSSStyleValue::CSSUnparsedValue(reify_a_list_of_component_values(vm, &segments))
        }
        Reification::List(values) => {
            // NB: <transform-list> is a StyleValueList that contains TransformStyleValues. If that's what we are,
            //     follow the steps for reifying that.
            if !values.is_empty()
                && values.iter().all(RetainedStyleValue::is_transformation)
                && let Some(transform_list) = reify_a_transform_list(vm, &values)
            {
                return AnyCSSStyleValue::CSSTransformValue(transform_list);
            }

            // NB: A list with a single item, such as `text-decoration-line: underline`, is that item on its own. Reify
            //     the item, so that a single keyword reifies as a CSSKeywordValue.
            if let [item] = values.as_slice() {
                return reify(vm, item, property);
            }

            // NB: Otherwise, there isn't an equivalent CSSStyleValue for StyleValueList, so just use the default.
            default_reification(vm, value, property)
        }
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-transform-list
fn reify_a_transform_list(vm: &Vm, values: &[RetainedStyleValue]) -> Option<Ref<CSSTransformValue>> {
    let transform_components: MarkedVec<'_, AnyCSSTransformComponent> = MarkedVec::new(vm);
    for transform in values {
        // NB: Not all transform functions are reifiable, in which case we give up reifying as a transform list.
        transform_components.push(reify_a_transform_function(vm, transform)?);
    }
    Some(CSSTransformValue::create(vm, &transform_components))
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-list-of-component-values
fn reify_a_list_of_component_values(vm: &Vm, segments: &[ReifiedUnresolvedSegment]) -> Ref<CSSUnparsedValue> {
    let members = MarkedVec::new(vm);
    for segment in segments {
        members.push(match segment {
            ReifiedUnresolvedSegment::Text(text) => {
                CSSUnparsedSegment::USVString(UsvString::from_utf16_replacing_lone_surrogates(text))
            }
            ReifiedUnresolvedSegment::Variable { name, fallback } => {
                let fallback = fallback
                    .as_ref()
                    .map(|fallback| reify_a_list_of_component_values(vm, fallback));
                CSSUnparsedSegment::CSSVariableReferenceValue(CSSVariableReferenceValue::create(
                    vm,
                    Utf16String::from_utf16(name),
                    fallback,
                ))
            }
        });
    }
    CSSUnparsedValue::create(vm, &members)
}

fn unit_name_of(node: &CalcReificationNode) -> &'static str {
    match node.numeric_kind {
        0 => "number",
        1 => DimensionType::Angle.unit_name(node.unit),
        2 => DimensionType::Flex.unit_name(node.unit),
        3 => DimensionType::Frequency.unit_name(node.unit),
        4 => DimensionType::Length.unit_name(node.unit),
        5 => "percent",
        6 => DimensionType::Resolution.unit_name(node.unit),
        7 => DimensionType::Time.unit_name(node.unit),
        kind => unreachable!("invalid numeric kind {kind}"),
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-math-expression
pub(crate) fn reify_a_math_expression(vm: &Vm, calculated: &RetainedStyleValue) -> Option<AnyCSSNumericValue> {
    let (nodes, child_indices) = calculation_nodes(calculated)?;
    let reified_nodes: MarkedVec<'_, AnyCSSNumericValue> = MarkedVec::with_capacity(vm, nodes.len());
    for node in &nodes {
        let numeric_type = node.numeric_type.to_calc();
        let children = &child_indices[node.child_start..node.child_start + node.child_count];
        let child = |index: usize| {
            reified_nodes
                .get(children[index])
                .expect("a child comes before its parent")
        };

        // NB: The reified objects get the types that their constructors would give them, computed from their reified
        //     children. The internal calculation's type may differ, for example by carrying a percent hint from the
        //     property that it belongs to.
        let combined_type_of_children = |combine: fn(&NumericType, &NumericType) -> Option<NumericType>| {
            let mut result: Option<NumericType> = None;
            for index in 0..children.len() {
                let child_type = child(index).numeric_type();
                match result {
                    Some(numeric_type_so_far) => match combine(&numeric_type_so_far, &child_type) {
                        Some(combined) => result = Some(combined),
                        None => return numeric_type,
                    },
                    None => result = Some(child_type),
                }
            }
            result.unwrap_or(numeric_type)
        };
        let added_types_of_children = || combined_type_of_children(NumericType::added_to);
        let multiplied_types_of_children = || combined_type_of_children(NumericType::multiplied_by);
        let reify_children = || {
            let values = MarkedVec::with_capacity(vm, children.len());
            for index in 0..children.len() {
                values.push(child(index));
            }
            CSSNumericArray::create(vm, &values)
        };

        reified_nodes.push(match node.kind {
            0 => AnyCSSNumericValue::CSSUnitValue(CSSUnitValue::create(vm, node.value, unit_name_of(node))),
            2 => AnyCSSNumericValue::CSSMathSum(CSSMathSum::create(vm, added_types_of_children(), reify_children())),
            3 => AnyCSSNumericValue::CSSMathProduct(CSSMathProduct::create(
                vm,
                multiplied_types_of_children(),
                reify_children(),
            )),
            4 => AnyCSSNumericValue::CSSMathNegate(CSSMathNegate::create(vm, child(0))),
            5 => AnyCSSNumericValue::CSSMathInvert(CSSMathInvert::create(vm, child(0))),
            6 => AnyCSSNumericValue::CSSMathMin(CSSMathMin::create(vm, added_types_of_children(), reify_children())),
            7 => AnyCSSNumericValue::CSSMathMax(CSSMathMax::create(vm, added_types_of_children(), reify_children())),
            8 => AnyCSSNumericValue::CSSMathClamp(CSSMathClamp::create(
                vm,
                added_types_of_children(),
                child(0),
                child(1),
                child(2),
            )),
            kind => unreachable!("invalid reification node kind {kind}"),
        });
    }
    reified_nodes.pop()
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-numeric-value
pub(crate) fn reify_a_numeric_value(vm: &Vm, numeric_value: &RetainedStyleValue) -> Option<AnyCSSNumericValue> {
    reify(vm, numeric_value, &Utf16String::default()).as_numeric()
}
