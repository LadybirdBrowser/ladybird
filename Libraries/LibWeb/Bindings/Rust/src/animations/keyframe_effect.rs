/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! KeyframeEffect, https://www.w3.org/TR/web-animations-1/#the-keyframeeffect-interface

use core::cell::RefCell;
use core::ffi::c_void;
use core::ptr::{NonNull, null_mut};

use ak::Utf16String;
use libjs_rust::api::{
    ErrorKind, ErrorType, Gc, MarkedVec, Object, PrimitiveString, PropertyKey, PropertyKind, Realm, Trace, Value,
    Visitor, Vm, get_iterator_from_method_impl, iterator_step_value,
};
pub(crate) use libweb_rust::web_animations::CompositeOperation;
use libweb_rust::web_animations::{
    CompositeOperationOrAuto, CssKeyframeValues, EffectRecordRef, KeyframePropertyName, KeyframeSetInput,
    KeyframeSetRef, PROPERTY_ID_CUSTOM, PROPERTY_ID_FLOAT, PROPERTY_ID_OFFSET, ParsedEasing, ParsedPseudoElement,
    StyleValue, camel_case_property_name, clone_property_value, is_animatable_property, parse_easing_function,
    parse_property_value, parse_pseudo_element_selector, property_id_from_camel_case, serialize_property_value,
};

use super::animation_effect::{
    AnimationEffect, EffectTiming, UnrestrictedDoubleOrCSSNumericValueOrDOMString, to_optional_effect_timing,
    units_equal,
};
use crate::bindings::conversions::{CppInterface, iterator_method};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, Exception, WebIdlResult};

unsafe extern "C" {
    fn web_animation_effect_set_target(effect: *const c_void, element: *mut c_void, update: bool);
    fn web_animation_effect_set_keyframe_set(effect: *const c_void, keyframe_set: *const c_void);
    fn web_animation_element_of_wrapper(wrapper: *mut c_void) -> *mut c_void;
    fn web_animation_wrap_element(realm: *mut c_void, element: *mut c_void) -> *mut c_void;
}

// https://www.w3.org/TR/web-animations-1/#dictdef-keyframeeffectoptions
pub(crate) struct KeyframeEffectOptions {
    pub(crate) parent: EffectTiming,
    pub(crate) composite: CompositeOperation,
    pub(crate) pseudo_element: Option<Utf16String>,
}

/// The union (unrestricted double or KeyframeEffectOptions) of the options of the KeyframeEffect constructor.
pub(crate) enum UnrestrictedDoubleOrKeyframeEffectOptions {
    UnrestrictedDouble(f64),
    KeyframeEffectOptions(KeyframeEffectOptions),
}

/// A value of a member of a property-indexed keyframe, which may be a list.
enum MaybeList<T> {
    Single(T),
    List(Vec<T>),
}

impl<T> MaybeList<T> {
    fn into_list(self) -> Vec<T> {
        match self {
            Self::Single(value) => vec![value],
            Self::List(values) => values,
        }
    }
}

// https://www.w3.org/TR/web-animations-1/#dictdef-basepropertyindexedkeyframe
// NB: This is an intermediate structure used only when parsing Keyframes provided by the caller in a slightly
//     different format. It is converted to BaseKeyframe, which is why it doesn't need to store the parsed properties.
struct BasePropertyIndexedKeyframe {
    offset: MaybeList<Option<f64>>,
    easing: MaybeList<Utf16String>,
    composite: MaybeList<CompositeOperationOrAuto>,
    properties: Vec<(Utf16String, Vec<Utf16String>)>,
}

impl Default for BasePropertyIndexedKeyframe {
    fn default() -> Self {
        Self {
            offset: MaybeList::List(Vec::new()),
            easing: MaybeList::List(Vec::new()),
            composite: MaybeList::List(Vec::new()),
            properties: Vec::new(),
        }
    }
}

// https://www.w3.org/TR/web-animations-1/#dictdef-basekeyframe
// NB: A keyframe before its property values and its easing are parsed.
struct BaseKeyframe {
    offset: Option<f64>,
    easing: Utf16String,
    composite: CompositeOperationOrAuto,
    computed_offset: Option<f64>,
    properties: Vec<(Utf16String, Utf16String)>,
}

impl Default for BaseKeyframe {
    fn default() -> Self {
        Self {
            offset: None,
            easing: Utf16String::from_ascii(b"linear"),
            composite: CompositeOperationOrAuto::Auto,
            computed_offset: None,
            properties: Vec::new(),
        }
    }
}

/// A keyframe of a KeyframeEffect, https://drafts.csswg.org/web-animations-1/#keyframe, with its property values and
/// its easing parsed.
pub(crate) struct ProcessedKeyframe {
    offset: Option<f64>,
    computed_offset: f64,
    easing: ParsedEasing,
    composite: CompositeOperationOrAuto,
    properties: Vec<(KeyframePropertyName, StyleValue)>,
}

impl From<CssKeyframeValues> for ProcessedKeyframe {
    fn from(keyframe: CssKeyframeValues) -> Self {
        Self {
            offset: Some(keyframe.offset),
            computed_offset: keyframe.offset,
            easing: keyframe.easing,
            composite: keyframe.composite,
            properties: keyframe.properties,
        }
    }
}

impl Clone for ProcessedKeyframe {
    fn clone(&self) -> Self {
        Self {
            offset: self.offset,
            computed_offset: self.computed_offset,
            easing: self.easing.clone(),
            composite: self.composite,
            properties: self
                .properties
                .iter()
                .map(|(property, value)| (property.clone(), clone_property_value(value)))
                .collect(),
        }
    }
}

fn type_error(message: String) -> Exception {
    Exception::TypeError(Utf16String::from_utf8(&message))
}

fn display(string: &Utf16String) -> String {
    String::from_utf16_lossy(&string.to_utf16())
}

fn throw<T>(vm: &Vm, error_type: ErrorType, value: Value) -> WebIdlResult<T> {
    Err(vm
        .throw_completion::<T>(ErrorKind::TypeError, error_type, &[&value])
        .err()
        .expect("throwing throws")
        .into())
}

fn convert_value_to_maybe_list<T>(
    vm: &Vm,
    value: Value,
    undefined_element: impl Fn() -> T,
    value_converter: impl Fn(Value) -> WebIdlResult<T>,
) -> WebIdlResult<MaybeList<T>> {
    // NB: As the union conversion rules of Web IDL have it, an object with an @@iterator method converts to the
    //     sequence, each element as the iterator yields it.
    if value.is_object()
        && let Some(method) = iterator_method(vm, value)?
    {
        let mut elements = Vec::new();
        let iterator = get_iterator_from_method_impl(vm, value, method)?;
        while let Some(element) = iterator_step_value(vm, &iterator)? {
            if element.is_undefined() {
                elements.push(undefined_element());
            } else {
                elements.push(value_converter(element)?);
            }
        }

        return Ok(MaybeList::List(elements));
    }

    Ok(MaybeList::Single(value_converter(value)?))
}

fn to_offset(vm: &Vm, value: Value) -> WebIdlResult<Option<f64>> {
    if value.is_nullish() {
        return Ok(None);
    }
    let double_value = value.to_double(vm)?;
    if double_value.is_nan() || double_value.is_infinite() {
        return Err(type_error(format!(
            "Invalid offset value: {}",
            display(&value.to_utf16_string(vm)?)
        )));
    }
    Ok(Some(double_value))
}

fn to_string(vm: &Vm, value: Value) -> WebIdlResult<Utf16String> {
    Ok(value.to_utf16_string(vm)?)
}

fn to_composite_operation(vm: &Vm, value: Value) -> WebIdlResult<CompositeOperationOrAuto> {
    if value.is_undefined() {
        return Ok(CompositeOperationOrAuto::Auto);
    }

    let string_value = value.to_utf16_string(vm)?;
    let string_value = string_value.to_utf16();
    if units_equal(&string_value, "replace") {
        return Ok(CompositeOperationOrAuto::Replace);
    }
    if units_equal(&string_value, "add") {
        return Ok(CompositeOperationOrAuto::Add);
    }
    if units_equal(&string_value, "accumulate") {
        return Ok(CompositeOperationOrAuto::Accumulate);
    }
    if units_equal(&string_value, "auto") {
        return Ok(CompositeOperationOrAuto::Auto);
    }

    Err(Exception::type_error("Invalid composite value"))
}

fn is_a_custom_property_name_string(name: &[u16]) -> bool {
    // A custom property is any property whose name starts with two dashes (U+002D HYPHEN-MINUS), like --foo.
    // The <custom-property-name> production corresponds to this: it’s defined as any <dashed-ident> (a valid
    // identifier that starts with two dashes), except -- itself, which is reserved for future use by CSS.
    name.len() > 2 && name[0] == u16::from(b'-') && name[1] == u16::from(b'-')
}

/// What steps 3 to 6 of the procedure to process a keyframe-like object make of the names of the properties of the
/// keyframe input: its animation properties, sorted.
fn animation_properties(vm: &Vm, keyframe_object: Gc<Object>) -> WebIdlResult<Vec<Utf16String>> {
    // 3. Build up a list of animatable properties as follows:
    //
    //    1. Let animatable properties be a list of property names (including shorthand properties that have longhand
    //       sub-properties that are animatable) that can be animated by the UA.
    //    2. Convert each property name in animatable properties to the equivalent IDL attribute by applying the
    //       animation property name to IDL attribute name algorithm.

    // 4. Let input properties be the result of calling the EnumerableOwnNames operation with keyframe input as the
    //    object.

    // 5. Make up a new list animation properties that consists of all of the properties that are in both input
    //    properties and animatable properties, or which are in input properties and conform to the
    //    <custom-property-name> production.
    let input_properties = keyframe_object.enumerable_own_property_names(vm, PropertyKind::Key)?;

    let mut animation_properties: Vec<Utf16String> = Vec::new();

    for index in 0..input_properties.len() {
        let input_property = input_properties.get(index).expect("the index is in the list");
        if !input_property.is_string() {
            continue;
        }

        let name = input_property.as_string().utf16_string();
        let units = name.to_utf16();

        // NB: Handle the two special cases.
        if units_equal(&units, "cssFloat") || units_equal(&units, "cssOffset") {
            animation_properties.push(name.clone());
        } else if units_equal(&units, "float") || units_equal(&units, "offset") {
            // NB: Ignore these property names.
        } else if is_a_custom_property_name_string(&units) {
            animation_properties.push(name.clone());
        } else if let Some(property) = property_id_from_camel_case(&name)
            && is_animatable_property(property)
        {
            animation_properties.push(name.clone());
        }
    }

    // 6. Sort animation properties in ascending order by the Unicode codepoints that define each property name.
    animation_properties.sort_by(|a, b| a.to_utf16().cmp(&b.to_utf16()));

    Ok(animation_properties)
}

/// The value of the member `name` of the keyframe input, or the string `default`, where there is one and the member is
/// undefined.
/// NB: The dictionary conversion gets each member and converts it before it gets the next, in the order composite,
///     easing, offset.
fn keyframe_member(vm: &Vm, keyframe_object: Gc<Object>, name: &str, default: Option<&[u8]>) -> WebIdlResult<Value> {
    let value = keyframe_object.get(vm, &PropertyKey::from_utf8(name))?;
    Ok(match default {
        Some(default) if value.is_undefined() => {
            Value::from_string(PrimitiveString::create(vm, Utf16String::from_ascii(default)))
        }
        _ => value,
    })
}

// https://www.w3.org/TR/web-animations-1/#process-a-keyframe-like-object
// NB: With the allow lists flag set to true.
fn process_a_keyframe_like_object_allowing_lists(
    vm: &Vm,
    keyframe_input: Value,
) -> WebIdlResult<BasePropertyIndexedKeyframe> {
    // 1. Run the procedure to convert an ECMAScript value to a dictionary type [WEBIDL] with keyframe input as the
    //    ECMAScript value, and the dictionary type depending on the value of the allow lists flag as follows:
    //
    //    -> If allow lists is true, use the following dictionary type: <BasePropertyIndexedKeyframe>.
    //    -> Otherwise, use the following dictionary type: <BaseKeyframe>.
    //
    //    Store the result of this procedure as keyframe output.
    let mut keyframe_output = BasePropertyIndexedKeyframe::default();
    // 2. If keyframe input is null or undefined, return keyframe output.
    if keyframe_input.is_nullish() {
        return Ok(keyframe_output);
    }

    let keyframe_object = keyframe_input.as_object();

    // NB: An undefined element of a list converts to the default value of its type.
    let composite = keyframe_member(vm, keyframe_object, "composite", Some(b"auto"))?;
    keyframe_output.composite = convert_value_to_maybe_list(
        vm,
        composite,
        || CompositeOperationOrAuto::Replace,
        |value| to_composite_operation(vm, value),
    )?;
    let easing = keyframe_member(vm, keyframe_object, "easing", Some(b"linear"))?;
    keyframe_output.easing =
        convert_value_to_maybe_list(vm, easing, Utf16String::default, |value| to_string(vm, value))?;
    let offset = keyframe_member(vm, keyframe_object, "offset", None)?;
    keyframe_output.offset = convert_value_to_maybe_list(vm, offset, || None, |value| to_offset(vm, value))?;

    // 3. through 6.
    let animation_properties = animation_properties(vm, keyframe_object)?;

    // 7. For each property name in animation properties:
    for property_name in animation_properties {
        // 1. Let raw value be the result of calling the [[Get]] internal method on keyframe input, with property name
        //    as the property key and keyframe input as the receiver.
        // 2. Check the completion record of raw value.
        let raw_value = keyframe_object.get(vm, &PropertyKey::from_utf16_string(&property_name))?;

        // 3. Convert raw value to a DOMString or to a sequence of DOMStrings property values as follows:

        // -> If allow lists is true,
        //    Let property values be the result of converting raw value to IDL type (DOMString or sequence<DOMString>)
        //    using the procedures defined for converting an ECMAScript value to an IDL value [WEBIDL].
        //
        //    If property values is a single DOMString, replace property values with a sequence of DOMStrings with the
        //    original value of property values as the only element.
        let property_values =
            convert_value_to_maybe_list(vm, raw_value, Utf16String::default, |value| to_string(vm, value))?.into_list();

        // 4. Calculate the normalized property name as the result of applying the IDL attribute name to animation
        //    property name algorithm to property name.
        // NB: We do not need to do this, since we did not need to do the reverse step (animation property name to IDL
        //     attribute name) in the steps above.

        // 5. Add a property to keyframe output with normalized property name as the property name, and property values
        //    as the property value.
        keyframe_output.properties.push((property_name, property_values));
    }

    Ok(keyframe_output)
}

// https://www.w3.org/TR/web-animations-1/#process-a-keyframe-like-object
// NB: With the allow lists flag set to false.
fn process_a_keyframe_like_object(vm: &Vm, keyframe_input: Value) -> WebIdlResult<BaseKeyframe> {
    // 1. Run the procedure to convert an ECMAScript value to a dictionary type [WEBIDL] with keyframe input as the
    //    ECMAScript value, and the dictionary type depending on the value of the allow lists flag as follows:
    //
    //    -> If allow lists is true, use the following dictionary type: <BasePropertyIndexedKeyframe>.
    //    -> Otherwise, use the following dictionary type: <BaseKeyframe>.
    //
    //    Store the result of this procedure as keyframe output.
    let mut keyframe_output = BaseKeyframe::default();
    // 2. If keyframe input is null or undefined, return keyframe output.
    if keyframe_input.is_nullish() {
        return Ok(keyframe_output);
    }

    let keyframe_object = keyframe_input.as_object();
    let composite = keyframe_member(vm, keyframe_object, "composite", Some(b"auto"))?;
    keyframe_output.composite = to_composite_operation(vm, composite)?;
    let easing = keyframe_member(vm, keyframe_object, "easing", Some(b"linear"))?;
    keyframe_output.easing = to_string(vm, easing)?;
    let offset = keyframe_member(vm, keyframe_object, "offset", None)?;
    keyframe_output.offset = to_offset(vm, offset)?;

    // 3. through 6.
    let animation_properties = animation_properties(vm, keyframe_object)?;

    // 7. For each property name in animation properties:
    for property_name in animation_properties {
        // 1. Let raw value be the result of calling the [[Get]] internal method on keyframe input, with property name
        //    as the property key and keyframe input as the receiver.
        // 2. Check the completion record of raw value.
        let raw_value = keyframe_object.get(vm, &PropertyKey::from_utf16_string(&property_name))?;

        // 3. Convert raw value to a DOMString or to a sequence of DOMStrings property values as follows:

        // -> Otherwise,
        //    Let property values be the result of converting raw value to a DOMString using the procedure for
        //    converting an ECMAScript value to a DOMString [WEBIDL].
        let property_values = raw_value.to_utf16_string(vm)?;

        // 4. Calculate the normalized property name as the result of applying the IDL attribute name to animation
        //    property name algorithm to property name.
        // NB: We do not need to do this, since we did not need to do the reverse step (animation property name to IDL
        //     attribute name) in the steps above.

        // 5. Add a property to keyframe output with normalized property name as the property name, and property values
        //    as the property value.
        keyframe_output.properties.push((property_name, property_values));
    }

    Ok(keyframe_output)
}

// https://www.w3.org/TR/web-animations-1/#compute-missing-keyframe-offsets
fn compute_missing_keyframe_offsets(keyframes: &mut [BaseKeyframe]) {
    // 1. For each keyframe, in keyframes, let the computed keyframe offset of the keyframe be equal to its keyframe
    //    offset value.
    for keyframe in keyframes.iter_mut() {
        keyframe.computed_offset = keyframe.offset;
    }

    // 2. If keyframes contains more than one keyframe and the computed keyframe offset of the first keyframe in
    //    keyframes is null, set the computed keyframe offset of the first keyframe to 0.
    if keyframes.len() > 1 && keyframes[0].computed_offset.is_none() {
        keyframes[0].computed_offset = Some(0.0);
    }

    // 3. If the computed keyframe offset of the last keyframe in keyframes is null, set its computed keyframe offset
    //    to 1.
    if let Some(last) = keyframes.last_mut()
        && last.computed_offset.is_none()
    {
        last.computed_offset = Some(1.0);
    }

    // 4. For each pair of keyframes A and B where:
    //    - A appears before B in keyframes, and
    //    - A and B have a computed keyframe offset that is not null, and
    //    - all keyframes between A and B have a null computed keyframe offset,
    let find_next_index_of_keyframe_with_computed_offset = |keyframes: &[BaseKeyframe], starting_index: usize| {
        (starting_index..keyframes.len()).find(|&index| keyframes[index].computed_offset.is_some())
    };

    let Some(mut index_a) = find_next_index_of_keyframe_with_computed_offset(keyframes, 0) else {
        return;
    };
    let mut maybe_index_b = find_next_index_of_keyframe_with_computed_offset(keyframes, index_a + 1);

    while let Some(index_b) = maybe_index_b {
        // calculate the computed keyframe offset of each keyframe between A and B as follows:
        for keyframe_index in index_a + 1..index_b {
            // 1. Let offsetk be the computed keyframe offset of a keyframe k.
            let offset_a = keyframes[index_a]
                .computed_offset
                .expect("A has a computed keyframe offset");
            let offset_b = keyframes[index_b]
                .computed_offset
                .expect("B has a computed keyframe offset");

            // 2. Let n be the number of keyframes between and including A and B minus 1.
            #[expect(clippy::cast_precision_loss)]
            let n = (index_b - index_a) as f64;

            // 3. Let index refer to the position of keyframe in the sequence of keyframes between A and B such that the
            //    first keyframe after A has an index of 1.
            #[expect(clippy::cast_precision_loss)]
            let index = (keyframe_index - index_a) as f64;

            // 4. Set the computed keyframe offset of keyframe to offsetA + (offsetB − offsetA) × index / n.
            keyframes[keyframe_index].computed_offset = Some(offset_a + (offset_b - offset_a) * index / n);
        }

        index_a = index_b;
        maybe_index_b = find_next_index_of_keyframe_with_computed_offset(keyframes, index_b + 1);
    }
}

// https://www.w3.org/TR/web-animations-1/#loosely-sorted-by-offset
fn is_loosely_sorted_by_offset(keyframes: &[BaseKeyframe]) -> bool {
    // The list of keyframes for a keyframe effect must be loosely sorted by offset, which means that for each keyframe
    // in the list that has a keyframe offset that is not null, the offset must be greater than or equal to the offset
    // of the previous keyframe in the list with a keyframe offset that is not null, if any.

    let mut last_offset: Option<f64> = None;
    for keyframe in keyframes {
        let Some(offset) = keyframe.offset else {
            continue;
        };

        if last_offset.is_some_and(|last_offset| offset < last_offset) {
            return false;
        }

        last_offset = Some(offset);
    }

    true
}

/// The property a property name of a keyframe names, or none for a name the keyframe ignores.
fn keyframe_property_of_name(name: &Utf16String) -> Option<(KeyframePropertyName, u16)> {
    let units = name.to_utf16();
    // NB: Handle some special cases.
    if units_equal(&units, "cssFloat") {
        return Some((KeyframePropertyName::Id(PROPERTY_ID_FLOAT), PROPERTY_ID_FLOAT));
    }
    if units_equal(&units, "cssOffset") {
        return Some((KeyframePropertyName::Id(PROPERTY_ID_OFFSET), PROPERTY_ID_OFFSET));
    }
    if units_equal(&units, "float") || units_equal(&units, "offset") {
        // NB: Ignore these properties.
        return None;
    }
    if is_a_custom_property_name_string(&units) {
        return Some((KeyframePropertyName::Custom(name.clone()), PROPERTY_ID_CUSTOM));
    }
    property_id_from_camel_case(name).map(|property_id| (KeyframePropertyName::Id(property_id), property_id))
}

fn invalid_easing(easing: &Utf16String) -> Exception {
    type_error(format!("Invalid animation easing value: \"{}\"", display(easing)))
}

// https://www.w3.org/TR/web-animations-1/#process-a-keyframes-argument
pub(crate) fn process_a_keyframes_argument(
    vm: &Vm,
    object: Option<Gc<Object>>,
) -> WebIdlResult<Vec<ProcessedKeyframe>> {
    // 1. If object is null, return an empty sequence of keyframes.
    let Some(object) = object else {
        return Ok(Vec::new());
    };
    let object_value = Value::from_object(object);

    // 2. Let processed keyframes be an empty sequence of keyframes.
    let mut processed_keyframes: Vec<BaseKeyframe> = Vec::new();
    let mut unused_easings: Vec<Utf16String> = Vec::new();

    // 3. Let method be the result of GetMethod(object, @@iterator).
    // 4. Check the completion record of method.
    let method = iterator_method(vm, object_value)?;

    // 5. Perform the steps corresponding to the first matching condition below:

    // -> If method is not undefined,
    if let Some(method) = method {
        // 1. Let iter be GetIterator(object, method).
        // 2. Check the completion record of iter.
        let iter = get_iterator_from_method_impl(vm, object_value, method)?;

        // 3. Repeat:
        // 1. Let next be IteratorStep(iter).
        // 2. Check the completion record of next.
        // 3. If next is false abort this loop.
        // 4. Let nextItem be IteratorValue(next).
        // 5. Check the completion record of nextItem.
        // FIXME: The spec should be updated to use IteratorStepValue instead.
        while let Some(next_item) = iterator_step_value(vm, &iter)? {
            // 6. If Type(nextItem) is not Undefined, Null or Object, then throw a TypeError and abort these steps.
            if !next_item.is_nullish() && !next_item.is_object() {
                return throw(vm, ErrorType::NotAnObjectOrNull, next_item);
            }

            // 7. Append to processed keyframes the result of running the procedure to process a keyframe-like object
            //    passing nextItem as the keyframe input with the allow lists flag set to false.
            processed_keyframes.push(process_a_keyframe_like_object(vm, next_item)?);
        }
    }
    // -> Otherwise,
    else {
        // 1. Let property-indexed keyframe be the result of running the procedure to process a keyframe-like object
        //    passing object as the keyframe input with the allow lists flag set to true.
        let property_indexed_keyframe = process_a_keyframe_like_object_allowing_lists(vm, object_value)?;

        // 2. For each member, m, in property-indexed keyframe, perform the following steps:
        for (property_name, property_values) in &property_indexed_keyframe.properties {
            // 1. Let property name be the key for m.

            // 2. If property name is "composite", "easing", or "offset", skip the remaining steps in this loop and
            //    continue from the next member in property-indexed keyframe after m.
            // NB: This will never happen, since these fields have dedicated members on BasePropertyIndexedKeyframe.

            // 3. Let property values be the value for m.

            // 4. Let property keyframes be an empty sequence of keyframes.
            let mut property_keyframes: Vec<BaseKeyframe> = Vec::new();

            // 5. For each value, v, in property values perform the following steps:
            for value in property_values {
                // 1. Let k be a new keyframe with a null keyframe offset.
                let mut keyframe = BaseKeyframe::default();

                // 2. Add the property-value pair, property name → v, to k.
                keyframe.properties.push((property_name.clone(), value.clone()));

                // 3. Append k to property keyframes.
                property_keyframes.push(keyframe);
            }

            // 6. Apply the procedure to compute missing keyframe offsets to property keyframes.
            compute_missing_keyframe_offsets(&mut property_keyframes);

            // 7. Add keyframes in property keyframes to processed keyframes.
            processed_keyframes.extend(property_keyframes);
        }

        // 3. Sort processed keyframes by the computed keyframe offset of each keyframe in increasing order.
        processed_keyframes.sort_by(|a, b| {
            let a = a
                .computed_offset
                .expect("a property keyframe has a computed keyframe offset");
            let b = b
                .computed_offset
                .expect("a property keyframe has a computed keyframe offset");
            // NB: Offsets are never NaN, and adding zero makes -0 and +0 compare equal, as step 4 merges them.
            (a + 0.0).total_cmp(&(b + 0.0))
        });

        // 4. Merge adjacent keyframes in processed keyframes when they have equal computed keyframe offsets.
        // NB: The spec doesn't specify how to merge them, but WebKit seems to just override the properties of the
        //     earlier keyframe with the properties of the later keyframe.
        let mut i = 0;
        while i + 1 < processed_keyframes.len() {
            if processed_keyframes[i].computed_offset == processed_keyframes[i + 1].computed_offset {
                let keyframe_b = processed_keyframes.remove(i + 1);
                let keyframe_a = &mut processed_keyframes[i];
                keyframe_a.easing = keyframe_b.easing;
                keyframe_a.composite = keyframe_b.composite;
                for (name, value) in keyframe_b.properties {
                    match keyframe_a
                        .properties
                        .iter_mut()
                        .find(|(existing, _)| existing.to_utf16() == name.to_utf16())
                    {
                        Some((_, existing_value)) => *existing_value = value,
                        None => keyframe_a.properties.push((name, value)),
                    }
                }
            } else {
                i += 1;
            }
        }

        // 5. Let offsets be a sequence of nullable double values assigned based on the type of the "offset" member
        //    of the property-indexed keyframe as follows:
        //
        // -> sequence<double?>,
        //    The value of "offset" as-is.
        // -> double?,
        //    A sequence of length one with the value of "offset" as its single item, i.e. « offset »,
        let offsets = property_indexed_keyframe.offset.into_list();

        // 6. Assign each value in offsets to the keyframe offset of the keyframe with the corresponding position in
        //    processed keyframes until the end of either sequence is reached.
        for (keyframe, offset) in processed_keyframes.iter_mut().zip(offsets) {
            keyframe.offset = offset;
        }

        // 7. Let easings be a sequence of DOMString values assigned based on the type of the "easing" member of the
        //    property-indexed keyframe as follows:
        //
        // -> sequence<DOMString>,
        //    The value of "easing" as-is.
        // -> DOMString,
        //    A sequence of length one with the value of "easing" as its single item, i.e. « easing »,
        let mut easings = property_indexed_keyframe.easing.into_list();

        // 8. If easings is an empty sequence, let it be a sequence of length one containing the single value "linear",
        //    i.e. « "linear" ».
        if easings.is_empty() {
            easings.push(Utf16String::from_ascii(b"linear"));
        }

        // 9. If easings has fewer items than processed keyframes, repeat the elements in easings successively starting
        //    from the beginning of the list until easings has as many items as processed keyframes.
        //
        //    For example, if processed keyframes has five items, and easings is the sequence « "ease-in", "ease-out" »,
        //    easings would be repeated to become « "ease-in", "ease-out", "ease-in", "ease-out", "ease-in" ».
        let num_easings = easings.len();
        let mut index = 0;
        while easings.len() < processed_keyframes.len() {
            easings.push(easings[index % num_easings].clone());
            index += 1;
        }

        // 10. If easings has more items than processed keyframes, store the excess items as unused easings.
        while easings.len() > processed_keyframes.len() {
            unused_easings.push(easings.pop().expect("easings has more items than processed keyframes"));
        }

        // 11. Assign each value in easings to a property named "easing" on the keyframe with the corresponding position
        //     in processed keyframes until the end of processed keyframes is reached.
        for (keyframe, easing) in processed_keyframes.iter_mut().zip(easings) {
            keyframe.easing = easing;
        }

        // 12. If the "composite" member of the property-indexed keyframe is not an empty sequence:
        let composite_value = property_indexed_keyframe.composite;
        if !matches!(&composite_value, MaybeList::List(values) if values.is_empty()) {
            // 1. Let composite modes be a sequence of CompositeOperationOrAuto values assigned from the "composite"
            //    member of property-indexed keyframe. If that member is a single CompositeOperationOrAuto value
            //    operation, let composite modes be a sequence of length one, with the value of the "composite" as its
            //    single item.
            let mut composite_modes = composite_value.into_list();

            // 2. As with easings, if composite modes has fewer items than processed keyframes, repeat the elements in
            //    composite modes successively starting from the beginning of the list until composite modes has as
            //    many items as processed keyframes.
            let num_composite_modes = composite_modes.len();
            index = 0;
            while composite_modes.len() < processed_keyframes.len() {
                composite_modes.push(composite_modes[index % num_composite_modes]);
                index += 1;
            }

            // 3. Assign each value in composite modes that is not auto to the keyframe-specific composite operation on
            //    the keyframe with the corresponding position in processed keyframes until the end of processed
            //    keyframes is reached.
            for (keyframe, composite_mode) in processed_keyframes.iter_mut().zip(composite_modes) {
                if composite_mode != CompositeOperationOrAuto::Auto {
                    keyframe.composite = composite_mode;
                }
            }
        }
    }

    // 6. If processed keyframes is not loosely sorted by offset, throw a TypeError and abort these steps.
    if !is_loosely_sorted_by_offset(&processed_keyframes) {
        return Err(Exception::type_error(
            "Keyframes are not in ascending order based on offset",
        ));
    }

    // 7. If there exist any keyframe in processed keyframes whose keyframe offset is non-null and less than zero or
    //    greater than one, throw a TypeError and abort these steps.
    for (i, keyframe) in processed_keyframes.iter().enumerate() {
        let Some(offset) = keyframe.offset else {
            continue;
        };

        if !(0.0..=1.0).contains(&offset) {
            return Err(type_error(format!("Keyframe {i} has invalid offset value {offset}")));
        }
    }

    // FIXME: After processing the keyframe argument, we need to turn the set of keyframes into a set of computed
    //        keyframes using the procedure outlined in the second half of
    //        https://www.w3.org/TR/web-animations-1/#calculating-computed-keyframes. For now, just compute the
    //        missing keyframe offsets
    // NB: Step 8 maps the processed keyframes one to one, so their computed keyframe offsets carry over.
    compute_missing_keyframe_offsets(&mut processed_keyframes);

    // 8. For each frame in processed keyframes, perform the following steps:
    let mut keyframes = Vec::with_capacity(processed_keyframes.len());
    for keyframe in processed_keyframes {
        // 1. For each property-value pair in frame, parse the property value using the syntax specified for that
        //    property.
        //
        //    If the property value is invalid according to the syntax for the property, discard the property-value
        //    pair. User agents that provide support for diagnosing errors in content SHOULD produce an appropriate
        //    warning highlighting the invalid property value.
        let mut parsed_properties: Vec<(KeyframePropertyName, StyleValue)> = Vec::new();
        for (property_string, value_string) in &keyframe.properties {
            let Some((property, property_id)) = keyframe_property_of_name(property_string) else {
                continue;
            };

            if let Some(style_value) = parse_property_value(property_id, value_string) {
                parsed_properties.push((property, style_value));
            }
        }

        // 2. Let the easing function of frame be the result of parsing the "easing" property on frame using the CSS
        //    syntax defined for the easing member of the EffectTiming dictionary.
        //
        //    If parsing the "easing" property fails, throw a TypeError and abort this procedure.
        let easing = parse_easing_function(&keyframe.easing).ok_or_else(|| invalid_easing(&keyframe.easing))?;

        keyframes.push(ProcessedKeyframe {
            offset: keyframe.offset,
            computed_offset: keyframe
                .computed_offset
                .expect("a processed keyframe has a computed keyframe offset"),
            easing,
            composite: keyframe.composite,
            properties: parsed_properties,
        });
    }

    // 9. Parse each of the values in unused easings using the CSS syntax defined for the easing member of the
    //    EffectTiming dictionary, and if any of the values fail to parse, throw a TypeError and abort this procedure.
    for unused_easing in &unused_easings {
        if parse_easing_function(unused_easing).is_none() {
            return Err(invalid_easing(unused_easing));
        }
    }

    Ok(keyframes)
}

/// The keyframes of the effect as they are reflected to script, read before any script runs.
struct ReflectedKeyframe {
    offset: Option<f64>,
    computed_offset: f64,
    easing: Utf16String,
    composite: &'static str,
    properties: Vec<(Utf16String, Utf16String)>,
}

/// The result of pseudo-element parsing on the value.
// https://drafts.csswg.org/web-animations-1/#dom-keyframeeffect-pseudoelement
pub(super) fn pseudo_element_parsing(value: Option<&Utf16String>) -> WebIdlResult<Option<ParsedPseudoElement>> {
    let Some(value) = value else {
        return Ok(None);
    };
    match parse_pseudo_element_selector(value) {
        Some(pseudo_element) => Ok(Some(pseudo_element)),
        None => Err(Exception::syntax_error(Utf16String::from_utf8(&format!(
            "Invalid pseudo-element selector: \"{}\"",
            display(value)
        )))),
    }
}

/// The keyframes argument of the KeyframeEffect(target, keyframes, options) constructor, or the keyframes
/// Animatable.animate() processed from it before it constructs its effect.
pub(crate) enum KeyframesArgument {
    Unprocessed(Option<Gc<Object>>),
    Processed(Vec<ProcessedKeyframe>),
}

// https://www.w3.org/TR/web-animations-1/#the-keyframeeffect-interface
pub(crate) struct KeyframeEffect {
    effect: AnimationEffect,
    // https://www.w3.org/TR/web-animations-1/#keyframe
    keyframes: RefCell<Vec<ProcessedKeyframe>>,
}

// SAFETY: The keyframes reach no cell; the effect visits what it reaches.
unsafe impl Trace for KeyframeEffect {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        self.effect.trace(visitor);
    }
}

impl Inherits<AnimationEffect> for KeyframeEffect {
    fn parent(&self) -> &AnimationEffect {
        &self.effect
    }
}

impl Wrappable for KeyframeEffect {
    const TRACED_ON_OWNER_THREAD_ONLY: bool = true;
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        self.effect.relevant_realm()
    }

    const FINALIZES: bool = true;

    fn finalize(object: Ref<Self>) {
        // NB: The record can outlive the effect until what holds the record is swept.
        object.effect.record().set_object_cell(None);
    }
}

impl KeyframeEffect {
    /// A new effect with the default timing, no keyframes and no target.
    pub(crate) fn create(vm: &Vm) -> Ref<Self> {
        let object = Self::create_for_record(vm, EffectRecordRef::new());
        object
            .effect
            .record()
            .set_object_cell(NonNull::new(cell_into_abi(object)));
        object
    }

    /// The object of the effect whose state `record` holds, which notes no keyframes of its own.
    pub(crate) fn create_for_record(vm: &Vm, record: EffectRecordRef) -> Ref<Self> {
        allocate(vm, |_| Self {
            effect: AnimationEffect::from_record(record),
            keyframes: RefCell::new(Vec::new()),
        })
    }

    pub(crate) fn effect(&self) -> &AnimationEffect {
        &self.effect
    }

    fn record_ptr(&self) -> *const c_void {
        self.effect.record().as_host_ptr()
    }

    fn invalidate(&self) {
        self.effect.record().invalidate();
    }

    /// Sets the target element of the effect to the host's `DOM::Element`, or to none, invalidating what that changes
    /// where `update`.
    pub(crate) fn set_target_element(&self, element: *mut c_void, update: bool) {
        // SAFETY: The effect holds a reference to the record, and the element is null or a live element.
        unsafe { web_animation_effect_set_target(self.record_ptr(), element, update) };
    }

    fn set_target_pseudo_element(&self, pseudo_element: Option<&ParsedPseudoElement>) {
        self.effect.record().set_pseudo_element(pseudo_element);
        self.invalidate();
        // FIXME: We don't remove the animated style from the old target element as part of normal animated style update
        //        and it will remain "stuck" until its style is fully invalidated for some other reason.
    }

    fn set_composite_operation(&self, composite: CompositeOperation) {
        self.effect.record().set_composite(composite);
        self.invalidate();
    }

    /// Sets the keyframes of the effect to the processed keyframes.
    pub(crate) fn set_processed_keyframes(&self, keyframes: Vec<ProcessedKeyframe>) {
        self.set_processed_keyframes_copied_from(keyframes, None);
    }

    /// Sets the keyframes of the effect to the processed keyframes, which resolve what they reference against the
    /// style sheet of those of `source` where they are a copy of its keyframes.
    fn set_processed_keyframes_copied_from(&self, keyframes: Vec<ProcessedKeyframe>, source: Option<&EffectRecordRef>) {
        let inputs = keyframes
            .iter()
            .map(|keyframe| KeyframeSetInput {
                computed_offset: keyframe.computed_offset,
                composite: keyframe.composite,
                easing: &keyframe.easing,
                properties: &keyframe.properties,
            })
            .collect::<Vec<_>>();
        let keyframe_set = match source {
            Some(source) => KeyframeSetRef::from_keyframes_of_copy(&inputs, source),
            None => KeyframeSetRef::from_keyframes(&inputs),
        };
        drop(inputs);
        // NB: Keyframes script sets replace those of CSS for good.
        self.effect.record().note_keyframes_set_by_script();
        *self.keyframes.borrow_mut() = keyframes;
        // SAFETY: The effect holds a reference to the record, and hands over the reference to the keyframe set.
        unsafe { web_animation_effect_set_keyframe_set(self.record_ptr(), keyframe_set.into_host_ptr()) };
    }

    /// The bytes the effect holds, as Animation's memory report counts them.
    pub(crate) fn memory_size(&self) -> usize {
        size_of::<Self>() + self.keyframes.borrow().capacity() * size_of::<ProcessedKeyframe>()
    }

    // https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-keyframeeffect
    pub(crate) fn construct(
        cx: &mut CallContext,
        target: Option<CppInterface>,
        keyframes: Option<Gc<Object>>,
        options: UnrestrictedDoubleOrKeyframeEffectOptions,
    ) -> WebIdlResult<Ref<Self>> {
        let target = target.map_or(null_mut(), |target| {
            // SAFETY: The wrapper is that of a live Element.
            unsafe { web_animation_element_of_wrapper(target.wrapper().as_ptr().cast()) }
        });
        Self::construct_for_target(cx, target, KeyframesArgument::Unprocessed(keyframes), options)
    }

    /// The procedure of the KeyframeEffect(target, keyframes, options) constructor, of the host's `DOM::Element`
    /// `target`, or of none where it is null.
    pub(crate) fn construct_for_target(
        cx: &mut CallContext,
        target: *mut c_void,
        keyframes: KeyframesArgument,
        options: UnrestrictedDoubleOrKeyframeEffectOptions,
    ) -> WebIdlResult<Ref<Self>> {
        let vm = cx.vm();

        // 1. Create a new KeyframeEffect object, effect.
        let effect = Self::create(vm);

        // 2. Set the target element of effect to target.
        effect.set_target_element(target, true);

        // 3. Set the target pseudo-selector to the result corresponding to the first matching condition below:

        //    If options is a KeyframeEffectOptions object with a pseudoElement property,
        if let UnrestrictedDoubleOrKeyframeEffectOptions::KeyframeEffectOptions(options) = &options {
            // Set the target pseudo-selector to the value of the pseudoElement property.
            //
            // When assigning this property, the error-handling defined for the pseudoElement setter on the interface
            // is applied. If the setter requires an exception to be thrown, this procedure must throw the same
            // exception and abort all further steps.
            effect.set_pseudo_element(cx, options.pseudo_element.clone())?;
        }
        //     Otherwise,
        //         Set the target pseudo-selector to null.
        // NB: This is the default when constructed.

        // 4. Let timing input be the result corresponding to the first matching condition below:
        let (timing_input, composite) = match options {
            //     If options is a KeyframeEffectOptions object,
            //         Let timing input be options.
            UnrestrictedDoubleOrKeyframeEffectOptions::KeyframeEffectOptions(options) => {
                (options.parent, Some(options.composite))
            }
            //     Otherwise (if options is a double),
            //         Let timing input be a new EffectTiming object with all members set to their default values and
            //         duration set to options.
            UnrestrictedDoubleOrKeyframeEffectOptions::UnrestrictedDouble(duration) => (
                EffectTiming {
                    delay: 0.0,
                    direction: super::animation_effect::PlaybackDirection::Normal,
                    duration: UnrestrictedDoubleOrCSSNumericValueOrDOMString::UnrestrictedDouble(duration),
                    easing: Utf16String::from_ascii(b"linear"),
                    end_delay: 0.0,
                    fill: super::animation_effect::FillMode::Auto,
                    iteration_start: 0.0,
                    iterations: 1.0,
                },
                None,
            ),
        };

        // https://drafts.csswg.org/web-animations-2/#the-effecttiming-dictionaries
        // Note: In this version of the spec, duration is not settable as a CSSNumericValue; however, duration may be
        //       returned as a CSSNumericValue when resolving the duration in getComputedTiming(). Future versions of
        //       the spec may enable setting the duration as a CSSNumericValue, where the unit is a valid time unit or
        //       percent.
        if matches!(
            timing_input.duration,
            UnrestrictedDoubleOrCSSNumericValueOrDOMString::CSSNumericValue(_)
        ) {
            return Err(Exception::type_error(
                "Setting duration as a CSSNumericValue is not supported",
            ));
        }

        // 5. Call the procedure to update the timing properties of an animation effect of effect from timing input.
        //    If that procedure causes an exception to be thrown, propagate the exception and abort this procedure.
        effect
            .effect
            .update_the_timing_properties(to_optional_effect_timing(timing_input))?;

        // 6. If options is a KeyframeEffectOptions object, assign the composite property of effect to the corresponding
        //    value from options.
        //
        //    When assigning this property, the error-handling defined for the corresponding setter on the KeyframeEffect
        //    interface is applied. If the setter requires an exception to be thrown for the value specified by options,
        //    this procedure must throw the same exception and abort all further steps.
        if let Some(composite) = composite {
            effect.set_composite_operation(composite);
        }

        // 7. Initialize the set of keyframes by performing the procedure defined for setKeyframes() passing keyframes as
        //    the input.
        match keyframes {
            KeyframesArgument::Unprocessed(keyframes) => effect.set_keyframes(cx, keyframes)?,
            KeyframesArgument::Processed(keyframes) => effect.set_processed_keyframes(keyframes),
        }

        Ok(effect)
    }

    // https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-keyframeeffect-source
    pub(crate) fn construct_from_source(cx: &mut CallContext, source: Ref<Self>) -> WebIdlResult<Ref<Self>> {
        // 1. Create a new KeyframeEffect object, effect.
        let effect = Self::create(cx.vm());

        // 2. Set the following properties of effect using the corresponding values of source:

        //   - effect target,
        let target = source
            .effect
            .record()
            .target_element()
            .map_or(null_mut(), NonNull::as_ptr);
        effect.set_target_element(target, false);
        effect.effect.record().copy_pseudo_element_from(source.effect.record());

        //   - keyframes,
        // NB: The keyframes are those the effect runs as well, which for keyframes of CSS are those its style sheet
        //     declares.
        if source.effect.record().keyframes_from_css() {
            let keyframes = source
                .effect
                .record()
                .css_keyframes_of_copy()
                .into_iter()
                .map(ProcessedKeyframe::from)
                .collect();
            effect.set_processed_keyframes_copied_from(keyframes, Some(source.effect.record()));
        } else {
            effect.set_processed_keyframes(source.keyframes.borrow().clone());
        }

        //   - composite operation, and
        effect.set_composite_operation(source.effect.record().composite());

        //   - all specified timing properties:

        // AD-HOC: This is defined in the web-animations level 1 spec so doesn't explicitly mention the level 2 properties
        //         (specified start delay, specified end delay, specified iteration duration) but it is required that we
        //         copy these and then normalize them as opposed to copying the already normalized values which may be
        //         invalid in this context i.e. if the existing effect was associated with a progress-based timeline

        //     - specified start delay,
        //     - specified end delay,
        //     - fill mode,
        //     - iteration start,
        //     - iteration count,
        //     - specified iteration duration,
        //     - playback direction, and
        //     - easing function.
        effect
            .effect
            .record()
            .copy_specified_timing_and_timing_function_from(source.effect.record());

        effect.effect.record().normalize_specified_timing();

        Ok(effect)
    }

    // https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-target
    pub(crate) fn target(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        let Some(element) = self.effect.record().target_element() else {
            return Ok(None);
        };
        // SAFETY: The realm is live, and the element is the live target of the effect.
        let wrapper = unsafe { web_animation_wrap_element(cx.realm().as_ptr().cast(), element.as_ptr()) };
        let wrapper = NonNull::new(wrapper.cast::<Object>()).expect("LibWeb returns the wrapper of the element");
        // SAFETY: LibWeb returns a live wrapper of an Element.
        Ok(Some(unsafe { CppInterface::from_wrapper(Gc::from_non_null(wrapper)) }))
    }

    pub(crate) fn set_target(&self, _cx: &mut CallContext, value: Option<CppInterface>) -> WebIdlResult<()> {
        let element = value.map_or(null_mut(), |target| {
            // SAFETY: The wrapper is that of a live Element.
            unsafe { web_animation_element_of_wrapper(target.wrapper().as_ptr().cast()) }
        });
        self.set_target_element(element, true);
        Ok(())
    }

    // https://drafts.csswg.org/web-animations-1/#dom-keyframeeffect-pseudoelement
    pub(crate) fn pseudo_element(&self, _cx: &mut CallContext) -> WebIdlResult<Option<Utf16String>> {
        Ok(self.effect.record().pseudo_element_serialization())
    }

    pub(crate) fn set_pseudo_element(&self, _cx: &mut CallContext, value: Option<Utf16String>) -> WebIdlResult<()> {
        // On setting, sets the target pseudo-selector of the animation effect to the result of
        // pseudo-element parsing on the provided value, defined as the following:
        // NB: The actual definition is in pseudo_element_parsing().
        let pseudo_element = pseudo_element_parsing(value.as_ref())?;
        self.set_target_pseudo_element(pseudo_element.as_ref());
        Ok(())
    }

    // https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-composite
    pub(crate) fn composite(&self, _cx: &mut CallContext) -> WebIdlResult<CompositeOperation> {
        self.effect.update_style_if_needed();
        Ok(self.effect.record().composite())
    }

    pub(crate) fn set_composite(&self, _cx: &mut CallContext, value: CompositeOperation) -> WebIdlResult<()> {
        self.set_composite_operation(value);
        Ok(())
    }

    // https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-getkeyframes
    pub(crate) fn get_keyframes<'vm>(&self, cx: &mut CallContext<'vm>) -> WebIdlResult<MarkedVec<'vm, Gc<Object>>> {
        let vm = cx.vm();
        let realm = cx.realm();

        self.effect.update_style_if_needed();

        // NB: The keyframes of a CSS animation or transition are those its style sheet or style change declares, as
        //     they compute over the style of the target.
        let css_keyframes: Vec<ProcessedKeyframe> = if self.effect.record().keyframes_from_css() {
            self.effect
                .record()
                .computed_css_keyframes()
                .into_iter()
                .map(ProcessedKeyframe::from)
                .collect()
        } else {
            Vec::new()
        };

        // NB: The keyframes are read before any script runs, as setting a property of a new object can call a setter
        //     on its prototype that changes them.
        let script_keyframes = self.keyframes.borrow();
        let keyframes_to_reflect: &[ProcessedKeyframe] = if self.effect.record().keyframes_from_css() {
            &css_keyframes
        } else {
            &script_keyframes
        };
        let reflected_keyframes: Vec<ReflectedKeyframe> = keyframes_to_reflect
            .iter()
            .map(|keyframe| ReflectedKeyframe {
                offset: keyframe.offset,
                computed_offset: keyframe.computed_offset,
                easing: keyframe.easing.serialization().clone(),
                composite: match keyframe.composite {
                    CompositeOperationOrAuto::Replace => "replace",
                    CompositeOperationOrAuto::Add => "add",
                    CompositeOperationOrAuto::Accumulate => "accumulate",
                    CompositeOperationOrAuto::Auto => "auto",
                },
                properties: keyframe
                    .properties
                    .iter()
                    .map(|(property, value)| {
                        // https://drafts.csswg.org/web-animations-1/#animation-property-name-to-idl-attribute-name
                        // 1. If property follows the <custom-property-name> production, return property.
                        // 2. If property refers to the CSS float property, return the string "cssFloat".
                        // 3. If property refers to the CSS offset property, return the string "cssOffset".
                        // 4. Otherwise, return the result of applying the CSS property to IDL attribute algorithm
                        //    [CSSOM] to property.
                        let key = match property {
                            KeyframePropertyName::Custom(name) => name.clone(),
                            KeyframePropertyName::Id(PROPERTY_ID_FLOAT) => Utf16String::from_ascii(b"cssFloat"),
                            KeyframePropertyName::Id(PROPERTY_ID_OFFSET) => Utf16String::from_ascii(b"cssOffset"),
                            KeyframePropertyName::Id(property_id) => {
                                Utf16String::from_utf8(camel_case_property_name(*property_id))
                            }
                        };
                        (key, serialize_property_value(value))
                    })
                    .collect(),
            })
            .collect();
        drop(script_keyframes);

        // NB: The getKeyframes() algorithm returns a fresh sequence of fresh objects on every invocation.
        let keyframes = MarkedVec::new(vm);
        let string = |string: Utf16String| Value::from_string(PrimitiveString::create(vm, string));
        for keyframe in reflected_keyframes {
            let object = Object::create(vm, realm, Some(realm.intrinsics().object_prototype(vm)));
            keyframes.push(object);
            // NB: The members of the keyframe and its animation properties are its own data properties, whatever the
            //     prototype of the object defines.
            let set = |key: &PropertyKey, value: Value| object.create_data_property_or_throw(vm, key, value);
            set(
                &PropertyKey::from_utf8("offset"),
                keyframe.offset.map_or(Value::NULL, Value::from_f64),
            )?;
            set(
                &PropertyKey::from_utf8("computedOffset"),
                Value::from_f64(keyframe.computed_offset),
            )?;
            set(&PropertyKey::from_utf8("easing"), string(keyframe.easing))?;
            set(
                &PropertyKey::from_utf8("composite"),
                string(Utf16String::from_ascii(keyframe.composite.as_bytes())),
            )?;
            for (key, value) in keyframe.properties {
                set(&PropertyKey::from_utf16_string(&key), string(value))?;
            }
        }
        Ok(keyframes)
    }

    // https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-setkeyframes
    pub(crate) fn set_keyframes(&self, cx: &mut CallContext, keyframes: Option<Gc<Object>>) -> WebIdlResult<()> {
        let keyframes = process_a_keyframes_argument(cx.vm(), keyframes)?;
        self.set_processed_keyframes(keyframes);
        Ok(())
    }
}
