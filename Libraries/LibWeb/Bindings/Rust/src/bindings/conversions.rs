/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Conversions between JS values and the Rust types of IDL values,
//! https://webidl.spec.whatwg.org/#es-type-mapping

use core::mem::MaybeUninit;
use core::ops::Range;
use std::borrow::Cow;

use ak::{Utf16String, Utf16StringUnits, convert_valid_utf16_to_utf8, utf8_length_from_valid_utf16};
use libjs_rust::api::{
    Array, ArrayBuffer, ArrayBufferOrder, DataView, ErrorKind, ErrorType, FunctionObject, Gc, MarkedVec, Must, Object,
    PrimitiveString, PropertyKey, Root, ThrowCompletionOr, Trace, TypedArrayBase, TypedArrayKind, Value, Vm,
    get_iterator_from_method_impl, get_view_byte_length, is_typed_array_out_of_bounds, is_view_out_of_bounds,
    iterator_step_value, make_data_view_with_buffer_witness_record, make_typed_array_with_buffer_witness_record,
    to_well_formed, typed_array_byte_length,
};

use crate::ak_format::ak_formatted;
use crate::bindings::CallContext;
use crate::bindings::interface::{Interface, Ref, create_dictionary_object, wrap};
use crate::bindings::services::web_bindings_object_implements_interface;

/// A USVString, a string without lone surrogates. Its UTF-16 code units are always well-formed.
#[derive(Clone, Eq, PartialEq)]
pub(crate) struct UsvString(Utf16String);

impl UsvString {
    pub(crate) fn from_ascii(string: &[u8]) -> Self {
        Self(Utf16String::from_ascii(string))
    }

    pub(crate) fn units(&self) -> Utf16StringUnits<'_> {
        self.0.as_units()
    }

    pub(crate) fn length_in_code_units(&self) -> usize {
        self.0.length_in_code_units()
    }

    pub(crate) fn utf8_length(&self) -> usize {
        match self.units() {
            Utf16StringUnits::Ascii(bytes) => bytes.len(),
            Utf16StringUnits::Utf16(code_units) => utf8_length_from_valid_utf16(code_units),
        }
    }

    /// Writes the string's UTF-8 encoding to the start of `output`, which need not be initialized, and returns the
    /// number of bytes written.
    ///
    /// # Safety
    ///
    /// `output` must have room for the encoding: utf8_length() bytes, or three bytes per code unit. The encoding is
    /// written without checking that, as measuring it first would cost a pass over the string.
    pub(crate) unsafe fn encode_utf8(&self, output: &mut [MaybeUninit<u8>]) -> usize {
        match self.units() {
            Utf16StringUnits::Ascii(bytes) => {
                output[..bytes.len()].write_copy_of_slice(bytes);
                bytes.len()
            }
            Utf16StringUnits::Utf16(code_units) => {
                debug_assert!(output.len() >= utf8_length_from_valid_utf16(code_units));
                // SAFETY: A USVString is well-formed, and the caller gives the output room for its encoding.
                unsafe { convert_valid_utf16_to_utf8(code_units, output) }
            }
        }
    }

    /// The string without its first code unit, if that is the ASCII character.
    pub(crate) fn strip_ascii_prefix(&self, prefix: u8) -> Option<Self> {
        match self.units() {
            Utf16StringUnits::Ascii(bytes) => bytes.strip_prefix(&[prefix]).map(Self::from_ascii),
            Utf16StringUnits::Utf16(code_units) => code_units
                .strip_prefix(&[u16::from(prefix)])
                .map(|rest| Self(Utf16String::from_utf16(rest))),
        }
    }

    pub(crate) fn into_utf16_string(self) -> Utf16String {
        self.0
    }

    pub(crate) fn from_utf8(string: &str) -> Self {
        Self(Utf16String::from_utf8(string))
    }

    pub(crate) fn to_utf8(&self) -> String {
        match self.units() {
            Utf16StringUnits::Ascii(bytes) => String::from_utf8(bytes.to_vec()),
            Utf16StringUnits::Utf16(code_units) => String::from_utf16(code_units).map_err(|_| unreachable!()),
        }
        .expect("a USVString is well-formed")
    }

    /// The string of the code units, with U+FFFD in place of every surrogate that is not part of a pair.
    pub(crate) fn from_utf16_replacing_lone_surrogates(code_units: &[u16]) -> Self {
        Self(to_well_formed(&Utf16String::from_utf16(code_units)))
    }
}

impl From<UsvString> for Utf16String {
    fn from(string: UsvString) -> Self {
        string.0
    }
}

/// A JS value converted to a DOMString, https://webidl.spec.whatwg.org/#es-DOMString
pub(crate) fn to_dom_string(vm: &Vm, value: Value) -> ThrowCompletionOr<Utf16String> {
    value.to_utf16_string(vm)
}

/// A JS value converted to a DOMString with [LegacyNullToEmptyString], https://webidl.spec.whatwg.org/#es-DOMString
pub(crate) fn to_dom_string_treating_null_as_empty(vm: &Vm, value: Value) -> ThrowCompletionOr<Utf16String> {
    // 1. If V is null and the conversion is to an IDL type associated with the [LegacyNullToEmptyString] extended
    //    attribute, then return the DOMString value that represents the empty string.
    if value.is_null() {
        return Ok(Utf16String::default());
    }
    to_dom_string(vm, value)
}

/// A JS value converted to a USVString, https://webidl.spec.whatwg.org/#es-USVString
pub(crate) fn to_usv_string(vm: &Vm, value: Value) -> ThrowCompletionOr<UsvString> {
    // 1. Let string be the result of converting V to a DOMString.
    let string = to_dom_string(vm, value)?;

    // 2. Return an IDL USVString value that is the result of converting string to a sequence of scalar values.
    Ok(UsvString(to_well_formed(&string)))
}

/// The value of an enumeration that a JS value names, https://webidl.spec.whatwg.org/#js-enumeration: the index of
/// the string that the value converts to among the values of the enumeration, which are ASCII.
pub(crate) fn to_enumeration_value(
    vm: &Vm,
    value: Value,
    enumeration_name: &str,
    enumeration_values: &[&str],
) -> ThrowCompletionOr<usize> {
    // 1. Let S be the result of calling ? ToString(V).
    let string = value.to_utf16_string(vm)?;

    // 2. If S is not one of E's enumeration values, then throw a TypeError.
    // 3. Return the enumeration value of type E that is equal to S.
    let index = enumeration_values
        .iter()
        .position(|enumeration_value| match string.as_units() {
            Utf16StringUnits::Ascii(units) => units == enumeration_value.as_bytes(),
            Utf16StringUnits::Utf16(units) => units.iter().copied().eq(enumeration_value.bytes().map(u16::from)),
        });
    match index {
        Some(index) => Ok(index),
        None => vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::InvalidEnumerationValue,
            &[&string, &enumeration_name],
        ),
    }
}

/// A value of an enumeration converted to a JS value, https://webidl.spec.whatwg.org/#js-enumeration
pub(crate) fn enumeration_value_to_js(cx: &CallContext, enumeration_value: &str) -> Value {
    Value::from_string(PrimitiveString::create_from_ascii(
        cx.vm(),
        enumeration_value.as_bytes(),
    ))
}

/// A JS value converted to a boolean, https://webidl.spec.whatwg.org/#js-boolean
#[expect(
    clippy::unnecessary_wraps,
    reason = "the generated bindings convert every type with an expression that can throw"
)]
pub(crate) fn to_boolean(value: Value) -> ThrowCompletionOr<bool> {
    // 1. Let x be the result of computing ToBoolean(V).
    // 2. Return the IDL boolean value that is the one that represents the same truth value as the JavaScript Boolean
    //    value x.
    Ok(value.to_boolean())
}

/// A JS value converted to a long, https://webidl.spec.whatwg.org/#js-long
pub(crate) fn to_long(vm: &Vm, value: Value) -> ThrowCompletionOr<i32> {
    // 1. Let x be ? ConvertToInt(V, 32, "signed"), which is ToInt32 without the extended attributes that change it.
    // 2. Return the IDL long value that represents the same numeric value as x.
    value.to_i32(vm)
}

/// A JS value converted to an unsigned long, https://webidl.spec.whatwg.org/#js-unsigned-long
pub(crate) fn to_unsigned_long(vm: &Vm, value: Value) -> ThrowCompletionOr<u32> {
    // 1. Let x be ? ConvertToInt(V, 32, "unsigned"), which is ToUint32 without the extended attributes that change
    //    it.
    // 2. Return the IDL unsigned long value that represents the same numeric value as x.
    value.to_u32(vm)
}

/// A JS value converted to an integer of the IDL type with [EnforceRange], whose bounds those are: steps 4 to 6 of
/// https://webidl.spec.whatwg.org/#abstract-opdef-converttoint, which throw the errors that WebIDL::convert_to_int()
/// of the C++ bindings throws.
fn to_integer_enforcing_range(vm: &Vm, value: Value, lower_bound: f64, upper_bound: f64) -> ThrowCompletionOr<f64> {
    // 4. Let x be ? ToNumber(V).
    // 5. If x is −0, then set x to +0.
    let x = value.to_double(vm)? + 0.0;

    // 6. If the conversion is to an IDL type associated with the [EnforceRange] extended attribute, then:
    //     1. If x is NaN, +∞, or −∞, then throw a TypeError.
    if !x.is_finite() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NumberIsNaNOrInfinity, &[]);
    }

    //     2. Set x to IntegerPart(x).
    let x = x.trunc() + 0.0;

    //     3. If x < lowerBound or x > upperBound, then throw a TypeError.
    if x < lower_bound || x > upper_bound {
        let message = format!(
            "Number '{}' is outside of allowed range of {} to {}",
            ak_formatted(x),
            ak_formatted(lower_bound),
            ak_formatted(upper_bound)
        );
        return vm.throw_completion_with_utf16_message(ErrorKind::TypeError, Utf16String::from_utf8(&message));
    }

    //     4. Return x.
    Ok(x)
}

/// The largest integer that a JS number represents unambiguously, the upper bound of a 64-bit integer type with
/// [EnforceRange] or [Clamp].
const MAX_SAFE_INTEGER: f64 = 9_007_199_254_740_991.0;

/// A JS value converted to a long with [EnforceRange], https://webidl.spec.whatwg.org/#js-long
#[allow(
    clippy::cast_possible_truncation,
    reason = "the value is an integer within the bounds of the type"
)]
pub(crate) fn to_long_enforcing_range(vm: &Vm, value: Value) -> ThrowCompletionOr<i32> {
    Ok(to_integer_enforcing_range(vm, value, f64::from(i32::MIN), f64::from(i32::MAX))? as i32)
}

/// A JS value converted to an unsigned long with [EnforceRange], https://webidl.spec.whatwg.org/#js-unsigned-long
#[allow(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "the value is an integer within the bounds of the type"
)]
pub(crate) fn to_unsigned_long_enforcing_range(vm: &Vm, value: Value) -> ThrowCompletionOr<u32> {
    Ok(to_integer_enforcing_range(vm, value, 0.0, f64::from(u32::MAX))? as u32)
}

/// A JS value converted to a long long with [EnforceRange], https://webidl.spec.whatwg.org/#js-long-long
#[allow(
    clippy::cast_possible_truncation,
    reason = "the value is an integer within the bounds of the type"
)]
pub(crate) fn to_long_long_enforcing_range(vm: &Vm, value: Value) -> ThrowCompletionOr<i64> {
    Ok(to_integer_enforcing_range(vm, value, -MAX_SAFE_INTEGER, MAX_SAFE_INTEGER)? as i64)
}

/// A JS value converted to an unsigned long long with [EnforceRange],
/// https://webidl.spec.whatwg.org/#js-unsigned-long-long
#[allow(
    clippy::cast_possible_truncation,
    clippy::cast_sign_loss,
    reason = "the value is an integer within the bounds of the type"
)]
pub(crate) fn to_unsigned_long_long_enforcing_range(vm: &Vm, value: Value) -> ThrowCompletionOr<u64> {
    Ok(to_integer_enforcing_range(vm, value, 0.0, MAX_SAFE_INTEGER)? as u64)
}

/// A JS value converted to an integer of the IDL type with [Clamp], whose bounds those are: steps 4, 5, 7 and 8 of
/// https://webidl.spec.whatwg.org/#abstract-opdef-converttoint
fn to_integer_clamping(vm: &Vm, value: Value, lower_bound: f64, upper_bound: f64) -> ThrowCompletionOr<f64> {
    // 4. Let x be ? ToNumber(V).
    // 5. If x is −0, then set x to +0.
    let x = value.to_double(vm)? + 0.0;

    // 7. If x is not NaN and the conversion is to an IDL type associated with the [Clamp] extended attribute, then:
    if !x.is_nan() {
        //     1. Set x to min(max(x, lowerBound), upperBound).
        //     2. Round x to the nearest integer, choosing the even integer if it lies halfway between two, and
        //        choosing +0 rather than −0.
        //     3. Return x.
        return Ok(x.clamp(lower_bound, upper_bound).round_ties_even() + 0.0);
    }

    // 8. If x is NaN, +0, +∞, or −∞, then return +0.
    Ok(0.0)
}

/// A JS value converted to a long long with [Clamp], https://webidl.spec.whatwg.org/#js-long-long
#[allow(
    clippy::cast_possible_truncation,
    reason = "the value is an integer within the bounds of the type"
)]
pub(crate) fn to_long_long_clamping(vm: &Vm, value: Value) -> ThrowCompletionOr<i64> {
    Ok(to_integer_clamping(vm, value, -MAX_SAFE_INTEGER, MAX_SAFE_INTEGER)? as i64)
}

/// The number that https://webidl.spec.whatwg.org/#abstract-opdef-converttoint has after step 10 for a bit length of
/// 64, as WebIDL::convert_to_int() of the C++ bindings computes it.
///
/// FIXME: The C++ bindings do the arithmetic in doubles, which cannot represent most integers near 2^64, so a
///        negative number does not wrap around to the integer the spec asks for: -1 becomes 2^64. The Rust bindings
///        do the same, so that an interface behaves the same in either language, until both are corrected together.
fn to_number_modulo_two_to_the_64(vm: &Vm, value: Value) -> ThrowCompletionOr<f64> {
    const TWO_TO_THE_64: f64 = 18_446_744_073_709_551_616.0;

    // 4. Let x be ? ToNumber(V).
    // 5. If x is −0, then set x to +0.
    let x = value.to_double(vm)?;

    // 8. If x is NaN, +0, +∞, or −∞, then return +0.
    if x.is_nan() || x == 0.0 || x.is_infinite() {
        return Ok(0.0);
    }

    // 9. Set x to IntegerPart(x).
    let x = x.trunc();

    // 10. Set x to x modulo 2^bitLength.
    let remainder = x % TWO_TO_THE_64;
    Ok(if remainder < 0.0 {
        remainder + TWO_TO_THE_64
    } else {
        remainder
    })
}

/// A JS value converted to a long long, https://webidl.spec.whatwg.org/#js-long-long
pub(crate) fn to_long_long(vm: &Vm, value: Value) -> ThrowCompletionOr<i64> {
    const TWO_TO_THE_63: f64 = 9_223_372_036_854_775_808.0;
    const TWO_TO_THE_64: f64 = 18_446_744_073_709_551_616.0;

    // 1. Let x be ? ConvertToInt(V, 64, "signed").
    let x = to_number_modulo_two_to_the_64(vm, value)?;

    // 11. If signedness is "signed" and x ≥ 2^(bitLength − 1), then return x − 2^(bitLength).
    // NB: The C++ bindings compare with the largest long long, which is 2^63 as a double, and with greater than.
    let x = if x > TWO_TO_THE_63 { x - TWO_TO_THE_64 } else { x };

    // 2. Return the IDL long long value that represents the same numeric value as x.
    #[allow(
        clippy::cast_possible_truncation,
        reason = "x is an integer of at most 2^63, which saturates"
    )]
    Ok(x as i64)
}

/// A JS value converted to an unsigned long long, https://webidl.spec.whatwg.org/#js-unsigned-long-long
pub(crate) fn to_unsigned_long_long(vm: &Vm, value: Value) -> ThrowCompletionOr<u64> {
    // 1. Let x be ? ConvertToInt(V, 64, "unsigned").
    let x = to_number_modulo_two_to_the_64(vm, value)?;

    // 2. Return the IDL unsigned long long value that represents the same numeric value as x.
    #[allow(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "x is an integer from 0 to 2^64, which saturates"
    )]
    Ok(x as u64)
}

/// A JS value converted to a float, https://webidl.spec.whatwg.org/#js-float
///
/// The identifier is the name of the parameter or member that the value is of, which the error names.
pub(crate) fn to_float(vm: &Vm, value: Value, identifier: &str) -> ThrowCompletionOr<f32> {
    // 1. Let x be ? ToNumber(V).
    let x = value.to_double(vm)?;

    // 2. If x is NaN, +∞, or −∞, then throw a TypeError.
    // 3. Let S be the set of finite IEEE 754 single-precision floating point values except −0, but with two special
    //    values added: 2^128 and −2^128.
    // 4. Let y be the number in S that is closest to x, selecting the number with an even significand if there are two
    //    equally close values. (The two special values 2^128 and −2^128 are considered to have even significands for
    //    this purpose.)
    // 5. If y is 2^128 or −2^128, then throw a TypeError.
    // 6. If y is +0 and x is negative, return −0.
    // 7. Return y.
    // NB: Rounding x to the nearest float gives an infinity where step 4 gives one of the two special values.
    #[allow(clippy::cast_possible_truncation, reason = "the spec rounds to the closest float")]
    let y = x as f32;
    if !y.is_finite() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::InvalidRestrictedFloatingPointParameter,
            &[&identifier],
        );
    }
    Ok(y)
}

/// A JS value converted to an unrestricted float, https://webidl.spec.whatwg.org/#js-unrestricted-float
pub(crate) fn to_unrestricted_float(vm: &Vm, value: Value) -> ThrowCompletionOr<f32> {
    // 1. Let x be ? ToNumber(V).
    let x = value.to_double(vm)?;

    // 2. If x is NaN, then return the IDL unrestricted float value that represents the IEEE 754 NaN value with the
    //    bit pattern 0x7fc00000.
    // 3. Let S be the set of finite IEEE 754 single-precision floating point values except −0, but with two special
    //    values added: 2^128 and −2^128.
    // 4. Let y be the number in S that is closest to x, selecting the number with an even significand if there are two
    //    equally close values. (The two special values 2^128 and −2^128 are considered to have even significands for
    //    this purpose.)
    // 5. If y is 2^128, return +∞.
    // 6. If y is −2^128, return −∞.
    // 7. If y is +0 and x is negative, return −0.
    // 8. Return y.
    // NB: Rounding x to the nearest float gives an infinity where step 4 gives one of the two special values.
    #[allow(clippy::cast_possible_truncation, reason = "the spec rounds to the closest float")]
    Ok(x as f32)
}

/// A JS value converted to a double, https://webidl.spec.whatwg.org/#js-double
///
/// The identifier is the name of the parameter or member that the value is of, which the error names.
pub(crate) fn to_double(vm: &Vm, value: Value, identifier: &str) -> ThrowCompletionOr<f64> {
    // 1. Let x be ? ToNumber(V).
    let x = value.to_double(vm)?;

    // 2. If x is NaN, +∞, or −∞, then throw a TypeError.
    if !x.is_finite() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::InvalidRestrictedFloatingPointParameter,
            &[&identifier],
        );
    }

    // 3. Return the IDL double value that represents the same numeric value as x.
    Ok(x)
}

/// A JS value converted to an unrestricted double, https://webidl.spec.whatwg.org/#js-unrestricted-double
pub(crate) fn to_unrestricted_double(vm: &Vm, value: Value) -> ThrowCompletionOr<f64> {
    // 1. Let x be ? ToNumber(V).
    // 2. If x is NaN, then return the IDL unrestricted double value that represents the IEEE 754 NaN value with the
    //    bit pattern 0x7ff8000000000000.
    // 3. Return the IDL unrestricted double value that represents the same numeric value as x.
    value.to_double(vm)
}

/// What the extended attributes of a buffer source type allow, https://webidl.spec.whatwg.org/#es-buffer-source-types
pub(crate) struct BufferSourceRules {
    pub(crate) allow_shared: bool,
    pub(crate) allow_resizable: bool,
}

/// A JS value converted to an IDL typed array value of the kind, whose name is the type's name.
pub(crate) fn to_typed_array(
    vm: &Vm,
    value: Value,
    kind: TypedArrayKind,
    name: &str,
    rules: &BufferSourceRules,
) -> ThrowCompletionOr<Gc<TypedArrayBase>> {
    // 1. Let T be the IDL type V is being converted to.
    // 2. If V is not an Object, or V does not have a [[TypedArrayName]] internal slot with a value equal to T's name,
    //    then throw a TypeError.
    let typed_array = value
        .is_object()
        .then(|| value.as_object().downcast::<TypedArrayBase>())
        .flatten()
        .filter(|typed_array| typed_array.kind() == kind);
    let Some(typed_array) = typed_array else {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&name]);
    };
    check_buffer_of_buffer_source(vm, &typed_array.viewed_array_buffer(), name, rules)?;

    // 5. Return the IDL value of type T that is a reference to the same object as V.
    Ok(typed_array)
}

/// The steps that the conversions to the buffer source types share, which check the buffer of the value against the
/// extended attributes of the type whose name is given.
fn check_buffer_of_buffer_source(
    vm: &Vm,
    buffer: &ArrayBuffer,
    type_name: &str,
    rules: &BufferSourceRules,
) -> ThrowCompletionOr<()> {
    // If the conversion is not to an IDL type associated with the [AllowShared] extended attribute, and
    // IsSharedArrayBuffer(V.[[ViewedArrayBuffer]]) is true, then throw a TypeError.
    if !rules.allow_shared && buffer.is_shared_array_buffer() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[]);
    }

    // If the conversion is not to an IDL type associated with the [AllowResizable] extended attribute, and
    // IsFixedLengthArrayBuffer(V.[[ViewedArrayBuffer]]) is false, then throw a TypeError.
    if !rules.allow_resizable && !buffer.is_fixed_length() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::NotAnObjectOfType,
            &[&format!("fixed-length {type_name}")],
        );
    }
    Ok(())
}

/// An IDL BufferSource value, https://webidl.spec.whatwg.org/#BufferSource
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum BufferSource {
    ArrayBuffer(Gc<ArrayBuffer>),
    DataView(Gc<DataView>),
    TypedArray(Gc<TypedArrayBase>),
}

/// An IDL ArrayBufferView value, https://webidl.spec.whatwg.org/#ArrayBufferView
#[derive(Clone, Copy)]
pub(crate) enum ArrayBufferView {
    DataView(Gc<DataView>),
    TypedArray(Gc<TypedArrayBase>),
}

impl From<ArrayBufferView> for BufferSource {
    fn from(view: ArrayBufferView) -> Self {
        match view {
            ArrayBufferView::DataView(view) => Self::DataView(view),
            ArrayBufferView::TypedArray(typed_array) => Self::TypedArray(typed_array),
        }
    }
}

pub(crate) struct BufferSourceExtent {
    pub(crate) buffer: Gc<ArrayBuffer>,
    pub(crate) is_out_of_bounds: bool,
    pub(crate) byte_offset: usize,
    pub(crate) byte_length: usize,
    pub(crate) element_size: usize,
}

impl BufferSource {
    pub(crate) fn extent(&self) -> BufferSourceExtent {
        match self {
            Self::TypedArray(typed_array) => {
                let record = make_typed_array_with_buffer_witness_record(typed_array, ArrayBufferOrder::SeqCst);
                // AD-HOC: The WebIDL spec has not been updated for resizable ArrayBuffer objects. This check follows
                //         the behavior of step 7 of the TypedArray.prototype.buffer getter.
                let is_out_of_bounds = is_typed_array_out_of_bounds(&record);
                let byte_length = if is_out_of_bounds {
                    0
                } else {
                    typed_array_byte_length(&record) as usize
                };
                BufferSourceExtent {
                    buffer: typed_array.viewed_array_buffer(),
                    is_out_of_bounds,
                    byte_offset: typed_array.byte_offset() as usize,
                    byte_length,
                    element_size: typed_array.element_size() as usize,
                }
            }
            Self::DataView(view) => {
                let record = make_data_view_with_buffer_witness_record(*view, ArrayBufferOrder::SeqCst);
                // AD-HOC: The WebIDL spec has not been updated for resizable ArrayBuffer objects. This check follows
                //         the behavior of step 7 of the DataView.prototype.buffer getter.
                let is_out_of_bounds = is_view_out_of_bounds(&record);
                let byte_length = if is_out_of_bounds {
                    0
                } else {
                    get_view_byte_length(&record) as usize
                };
                BufferSourceExtent {
                    buffer: view.viewed_array_buffer(),
                    is_out_of_bounds,
                    byte_offset: view.byte_offset() as usize,
                    byte_length,
                    element_size: 1,
                }
            }
            Self::ArrayBuffer(buffer) => BufferSourceExtent {
                buffer: *buffer,
                is_out_of_bounds: false,
                byte_offset: 0,
                byte_length: buffer.byte_length(),
                element_size: 1,
            },
        }
    }

    /// https://webidl.spec.whatwg.org/#dfn-get-buffer-source-copy, without copying the bytes unless their buffer is
    /// shared.
    ///
    /// # Safety
    ///
    /// The function must not run script or allocate a cell, either of which could detach, resize or free the buffer
    /// that the bytes are in.
    pub(crate) unsafe fn with_bytes<R>(&self, function: impl FnOnce(&[u8]) -> R) -> R {
        let extent = self.extent();
        // 7. If IsDetachedBuffer(jsArrayBuffer) is true, then return the empty byte sequence.
        let held_byte_length = if extent.buffer.is_detached() {
            0
        } else {
            extent.byte_length
        };
        let held_bytes = extent.byte_offset..extent.byte_offset + held_byte_length;
        // SAFETY: The caller keeps the function from running script and from allocating.
        unsafe { with_bytes_of_buffer(&extent.buffer, held_bytes, function) }
            .expect("a buffer has the bytes of its views that are in bounds")
    }
}

/// # Safety
///
/// The function must not run script or allocate a cell, either of which could detach, resize or free the buffer that
/// the bytes are in.
pub(crate) unsafe fn with_bytes_of_buffer<R>(
    buffer: &ArrayBuffer,
    bytes: Range<usize>,
    function: impl FnOnce(&[u8]) -> R,
) -> Option<R> {
    // SAFETY: The caller keeps the function from running script and from allocating.
    unsafe { borrowed_or_copied_bytes_of_buffer(buffer, bytes) }.map(|bytes| function(&bytes))
}

/// `None` when the buffer does not have the bytes.
pub(crate) fn copy_of_bytes_of_buffer(buffer: &ArrayBuffer, bytes: Range<usize>) -> Option<Vec<u8>> {
    // SAFETY: The bytes are borrowed only while they are copied, which neither runs script nor allocates a cell.
    unsafe { borrowed_or_copied_bytes_of_buffer(buffer, bytes) }.map(Cow::into_owned)
}

/// # Safety
///
/// Nothing may run script or allocate a cell while the bytes are borrowed, either of which could detach, resize or
/// free the buffer that they are in.
unsafe fn borrowed_or_copied_bytes_of_buffer(buffer: &ArrayBuffer, bytes: Range<usize>) -> Option<Cow<'_, [u8]>> {
    if bytes.is_empty() {
        return Some(Cow::Borrowed(&[]));
    }
    if buffer.is_detached() || bytes.end > buffer.byte_length() {
        return None;
    }

    // NB: Another thread can write the bytes of a shared buffer at any time, so they are copied.
    if buffer.is_shared_array_buffer() {
        return Some(Cow::Owned(buffer.copy_to_byte_buffer(bytes.start, bytes.len())));
    }
    let data = buffer.with_data_block(|data_block| data_block.data_at(bytes.start));
    assert!(!data.is_null());
    // SAFETY: The buffer is not detached and has the bytes, which nothing writes while they are borrowed, as the
    //         buffer is not shared and the caller keeps script from running.
    Some(Cow::Borrowed(unsafe { ak::slice_from_raw_parts(data, bytes.len()) }))
}

impl ArrayBufferView {
    /// # Safety
    ///
    /// The function must not run script or allocate a cell, either of which could detach, resize or free the buffer
    /// that the bytes are in.
    pub(crate) unsafe fn with_bytes<R>(&self, function: impl FnOnce(&[u8]) -> R) -> R {
        // SAFETY: The caller keeps the function from running script and from allocating.
        unsafe { BufferSource::from(*self).with_bytes(function) }
    }
}

/// A JS value converted to a BufferSource, with the steps of the conversion to a union type that apply to the buffer
/// source types, https://webidl.spec.whatwg.org/#js-union
pub(crate) fn to_buffer_source(vm: &Vm, value: Value, rules: &BufferSourceRules) -> ThrowCompletionOr<BufferSource> {
    if value.is_object()
        && let Some(buffer_source) = buffer_source_if(vm, value.as_object(), rules)?
    {
        return Ok(buffer_source);
    }

    // 20. Throw a TypeError.
    throw_no_union_types_matched(vm)
}

/// An object converted to a BufferSource if it is one, with the steps of the conversion to a union type that apply
/// to the buffer source types, https://webidl.spec.whatwg.org/#js-union
pub(crate) fn buffer_source_if(
    vm: &Vm,
    object: Gc<Object>,
    rules: &BufferSourceRules,
) -> ThrowCompletionOr<Option<BufferSource>> {
    // 6. If V is an Object, V has an [[ArrayBufferData]] internal slot, and IsSharedArrayBuffer(V) is false, then:
    //     1. If types includes ArrayBuffer, then return the result of converting V to ArrayBuffer.
    if let Some(buffer) = object.downcast::<ArrayBuffer>()
        && !buffer.is_shared_array_buffer()
    {
        check_buffer_of_buffer_source(vm, &buffer, "ArrayBuffer", rules)?;
        return Ok(Some(BufferSource::ArrayBuffer(buffer)));
    }

    // FIXME: 7. If V is an Object, V has an [[ArrayBufferData]] internal slot, and IsSharedArrayBuffer(V) is true,
    //           then:

    // 8. If V is an Object and V has a [[DataView]] internal slot, then:
    //     1. If types includes DataView, then return the result of converting V to DataView.
    if let Some(view) = object.downcast::<DataView>() {
        check_buffer_of_buffer_source(vm, &view.viewed_array_buffer(), "DataView", rules)?;
        return Ok(Some(BufferSource::DataView(view)));
    }

    // 9. If V is an Object and V has a [[TypedArrayName]] internal slot, then:
    //     1. If types includes a typed array type whose name is the value of V's [[TypedArrayName]] internal slot,
    //        then return the result of converting V to that type.
    if let Some(typed_array) = object.downcast::<TypedArrayBase>() {
        let type_name = format!("{:?}", typed_array.kind());
        check_buffer_of_buffer_source(vm, &typed_array.viewed_array_buffer(), &type_name, rules)?;
        return Ok(Some(BufferSource::TypedArray(typed_array)));
    }

    Ok(None)
}

/// A JS value converted to an ArrayBufferView, with the steps of the conversion to a union type that apply to the
/// types of the views, https://webidl.spec.whatwg.org/#js-union
pub(crate) fn to_array_buffer_view(
    vm: &Vm,
    value: Value,
    rules: &BufferSourceRules,
) -> ThrowCompletionOr<ArrayBufferView> {
    if value.is_object() {
        let object = value.as_object();

        // 8. If V is an Object and V has a [[DataView]] internal slot, then:
        //     1. If types includes DataView, then return the result of converting V to DataView.
        if let Some(view) = object.downcast::<DataView>() {
            check_buffer_of_buffer_source(vm, &view.viewed_array_buffer(), "DataView", rules)?;
            return Ok(ArrayBufferView::DataView(view));
        }

        // 9. If V is an Object and V has a [[TypedArrayName]] internal slot, then:
        //     1. If types includes a typed array type whose name is the value of V's [[TypedArrayName]] internal slot,
        //        then return the result of converting V to that type.
        if let Some(typed_array) = object.downcast::<TypedArrayBase>() {
            let type_name = format!("{:?}", typed_array.kind());
            check_buffer_of_buffer_source(vm, &typed_array.viewed_array_buffer(), &type_name, rules)?;
            return Ok(ArrayBufferView::TypedArray(typed_array));
        }
    }

    // 20. Throw a TypeError.
    throw_no_union_types_matched(vm)
}

/// The last step of the conversion of a JS value to a union type, https://webidl.spec.whatwg.org/#js-union, with
/// the message of the C++ bindings.
pub(crate) fn throw_no_union_types_matched<T>(vm: &Vm) -> ThrowCompletionOr<T> {
    // 20. Throw a TypeError.
    vm.throw_completion_with_message(ErrorKind::TypeError, "No union types matched".to_string())
}

/// Where an IDL sequence keeps its elements: a Vec, which hides cells from the collector, so its elements hold none,
/// or a MarkedVec, which keeps them alive while the rest are converted and for as long as the sequence lives.
pub(crate) trait SequenceStorage<T> {
    fn append(&mut self, value: T);
}

impl<T> SequenceStorage<T> for Vec<T> {
    fn append(&mut self, value: T) {
        self.push(value);
    }
}

impl<T: Trace + Clone + 'static> SequenceStorage<T> for MarkedVec<'_, T> {
    fn append(&mut self, value: T) {
        self.push(value);
    }
}

/// A JS value converted to an IDL sequence in `sequence`, https://webidl.spec.whatwg.org/#js-sequence, with the
/// conversion of its elements.
pub(crate) fn sequence_from_js<T, S: SequenceStorage<T>>(
    vm: &Vm,
    value: Value,
    sequence: S,
    element_from_js: impl FnMut(Value) -> ThrowCompletionOr<T>,
) -> ThrowCompletionOr<S> {
    // 1. If V is not an Object, throw a TypeError.
    if !value.is_object() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&value]);
    }

    // 2. Let method be ? GetMethod(V, %Symbol.iterator%).
    // 3. If method is undefined, throw a TypeError.
    let Some(method) = iterator_method(vm, value)? else {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotIterable, &[&value]);
    };

    // 4. Return the result of creating a sequence from V and method.
    sequence_from_iterable(vm, value, method, sequence, element_from_js)
}

/// ? GetMethod(V, %Symbol.iterator%), which decides whether a JS value converts to a sequence type.
pub(crate) fn iterator_method(vm: &Vm, value: Value) -> ThrowCompletionOr<Option<Gc<FunctionObject>>> {
    value.get_method(vm, &PropertyKey::from(vm.well_known_symbols().iterator))
}

/// An IDL sequence in `sequence` created from an iterable and its iterator getter method,
/// https://webidl.spec.whatwg.org/#create-sequence-from-iterable
pub(crate) fn sequence_from_iterable<T, S: SequenceStorage<T>>(
    vm: &Vm,
    iterable: Value,
    method: Gc<FunctionObject>,
    mut sequence: S,
    mut element_from_js: impl FnMut(Value) -> ThrowCompletionOr<T>,
) -> ThrowCompletionOr<S> {
    // 1. Let iteratorRecord be ? GetIteratorFromMethod(iterable, method).
    let iterator_record = get_iterator_from_method_impl(vm, iterable, method)?;

    // 2. Initialize i to be 0.
    // 3. Repeat
    //     1. Let next be ? IteratorStepValue(iteratorRecord).
    //     2. If next is done, then return an IDL sequence value of type sequence<T> of length i, where the value of the
    //        element at index j is Sj.
    while let Some(next) = iterator_step_value(vm, &iterator_record)? {
        //     3. Initialize Si to the result of converting next to an IDL value of type T.
        //     4. Set i to i + 1.
        sequence.append(element_from_js(next)?);
    }
    Ok(sequence)
}

/// A JS value converted to an IDL record, https://webidl.spec.whatwg.org/#js-record, with the conversions of its keys
/// and values. The entries are in a Vec in the order of the record, which hides cells from the collector, so their
/// types hold none.
pub(crate) fn record_from_js<K: PartialEq, V>(
    vm: &Vm,
    value: Value,
    mut key_from_js: impl FnMut(Value) -> ThrowCompletionOr<K>,
    mut value_from_js: impl FnMut(Value) -> ThrowCompletionOr<V>,
) -> ThrowCompletionOr<Vec<(K, V)>> {
    // 1. If O is not an Object, throw a TypeError.
    if !value.is_object() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&value]);
    }
    let object = value.as_object();

    // 2. Let result be a new empty instance of record<K, V>.
    let mut result: Vec<(K, V)> = Vec::new();

    // 3. Let keys be ? O.[[OwnPropertyKeys]]().
    let keys = object.internal_own_property_keys(vm)?;

    // 4. For each key of keys:
    for index in 0..keys.len() {
        let key = keys.get(index).expect("the index is in bounds");
        let property_key = PropertyKey::from_value(vm, key).must();

        // 1. Let desc be ? O.[[GetOwnProperty]](key).
        let descriptor = object.internal_get_own_property(vm, &property_key)?;

        // 2. If desc is not undefined and desc.[[Enumerable]] is true:
        if !descriptor.is_some_and(|descriptor| descriptor.enumerable == Some(true)) {
            continue;
        }

        // 1. Let typedKey be key converted to an IDL value of type K.
        let typed_key = key_from_js(key)?;

        // 2. Let value be ? Get(O, key).
        let value = object.get(vm, &property_key)?;

        // 3. Let typedValue be value converted to an IDL value of type V.
        let typed_value = value_from_js(value)?;

        // 4. Set result[typedKey] to typedValue.
        match result.iter_mut().find(|(existing_key, _)| *existing_key == typed_key) {
            Some(entry) => entry.1 = typed_value,
            None => result.push((typed_key, typed_value)),
        }
    }

    // 5. Return result.
    Ok(result)
}

/// The types of the IDL definitions that a JS value converts to, with the conversions that the generated bindings
/// write for them.
pub(crate) trait FromJs: Sized {
    fn from_js(vm: &Vm, value: Value) -> ThrowCompletionOr<Self>;
}

/// A dictionary without required members, which has a value that no JS value was converted to: the default value
/// `{}` of a parameter.
pub(crate) trait EmptyDictionary {
    fn empty() -> Self;
}

/// The object that the members of a dictionary are read from, step 1 of https://webidl.spec.whatwg.org/#js-dictionary.
/// It is None for undefined and null, which convert to a dictionary without reading a member.
pub(crate) fn dictionary_object(vm: &Vm, value: Value, dictionary_name: &str) -> ThrowCompletionOr<Option<Gc<Object>>> {
    // 1. If V is not Undefined, Null, or an Object, then throw a TypeError.
    if value.is_object() {
        return Ok(Some(value.as_object()));
    }
    if value.is_nullish() {
        return Ok(None);
    }
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&dictionary_name])
}

/// The value of a member of a dictionary, if the object it is read from has one that is not undefined, steps 4.1.2
/// and 4.1.3 of https://webidl.spec.whatwg.org/#js-dictionary
pub(crate) fn dictionary_member(
    vm: &Vm,
    object: Option<Gc<Object>>,
    key: &PropertyKey,
) -> ThrowCompletionOr<Option<Value>> {
    // 2. If V is Undefined or Null, then let jsMemberValue be undefined.
    let Some(object) = object else {
        return Ok(None);
    };

    // 3. Otherwise, let jsMemberValue be ? Get(V, key).
    let value = object.get(vm, key)?;
    Ok((!value.is_undefined()).then_some(value))
}

/// An IDL value converted to a JS value, https://webidl.spec.whatwg.org/#js-type-mapping
pub(crate) trait ToJs {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value>;
}

impl ToJs for () {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::UNDEFINED)
    }
}

impl ToJs for bool {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_bool(self))
    }
}

/// https://webidl.spec.whatwg.org/#js-any
impl ToJs for Value {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(self)
    }
}

/// An object converted to a JS value: https://webidl.spec.whatwg.org/#js-object, and what an implementation returns
/// for a promise type, https://webidl.spec.whatwg.org/#js-promise, which is the promise object.
impl ToJs for Gc<Object> {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_object(self))
    }
}

/// https://webidl.spec.whatwg.org/#js-long
impl ToJs for i32 {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_i32(self))
    }
}

/// https://webidl.spec.whatwg.org/#js-unsigned-long
impl ToJs for u32 {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_f64(f64::from(self)))
    }
}

/// https://webidl.spec.whatwg.org/#js-long-long
impl ToJs for i64 {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        #[allow(clippy::cast_precision_loss, reason = "the spec converts to the closest double")]
        Ok(Value::from_f64(self as f64))
    }
}

/// https://webidl.spec.whatwg.org/#js-unsigned-long-long
impl ToJs for u64 {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        #[allow(clippy::cast_precision_loss, reason = "the spec converts to the closest double")]
        Ok(Value::from_f64(self as f64))
    }
}

/// https://webidl.spec.whatwg.org/#js-float
impl ToJs for f32 {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_f64(f64::from(self)))
    }
}

/// https://webidl.spec.whatwg.org/#js-unrestricted-double
impl ToJs for f64 {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_f64(self))
    }
}

/// A callback function converted to a JS value, https://webidl.spec.whatwg.org/#js-callback-function
impl ToJs for Gc<FunctionObject> {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_object(self))
    }
}

impl ToJs for Utf16String {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_string(PrimitiveString::create(cx.vm(), self)))
    }
}

impl ToJs for UsvString {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        self.into_utf16_string().to_js(cx)
    }
}

impl ToJs for Gc<TypedArrayBase> {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_object(self))
    }
}

/// An object of an interface converted to a JS value, https://webidl.spec.whatwg.org/#js-interface
impl<T: Interface> ToJs for Ref<T> {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_object(wrap(cx, self)))
    }
}

/// An object of an interface that LibWeb implements in C++, as its wrapper. Only the C++ code knows the object
/// behind the wrapper, so an implementation hands the wrapper to a service of LibWeb to have something done with it.
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppInterface(Gc<Object>);

impl CppInterface {
    /// The object whose wrapper a service of LibWeb returned.
    ///
    /// # Safety
    ///
    /// `wrapper` must be a wrapper of an object of the interface that the IDL names where the value is used.
    pub(crate) unsafe fn from_wrapper(wrapper: Gc<Object>) -> Self {
        Self(wrapper)
    }

    pub(crate) fn wrapper(self) -> Gc<Object> {
        self.0
    }
}

/// Whether the object is a wrapper of an object that implements the interface, which LibWeb implements in C++ and
/// knows by the value of its Web::Bindings::InterfaceName.
pub(crate) fn implements_cpp_interface(object: Gc<Object>, interface_name: u16) -> bool {
    // SAFETY: The object is live.
    object.is_platform_object()
        && unsafe { web_bindings_object_implements_interface(object.as_ptr().cast(), interface_name) }
}

/// An object of an interface that LibWeb implements in C++ converted from a JS value,
/// https://webidl.spec.whatwg.org/#js-interface
pub(crate) fn to_cpp_interface(
    vm: &Vm,
    value: Value,
    interface_name: u16,
    name: &str,
) -> ThrowCompletionOr<CppInterface> {
    // 1. If V implements I, then return the IDL interface type value that represents a reference to that platform
    //    object.
    if value.is_object() && implements_cpp_interface(value.as_object(), interface_name) {
        return Ok(CppInterface(value.as_object()));
    }
    // 2. Throw a TypeError.
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&name])
}

/// A JS value converted to a union of interfaces that LibWeb implements in C++, which the values of their
/// Web::Bindings::InterfaceName name, https://webidl.spec.whatwg.org/#js-union
pub(crate) fn to_cpp_interface_in_union(
    vm: &Vm,
    value: Value,
    interface_names: &[u16],
) -> ThrowCompletionOr<CppInterface> {
    // 5. If V is a platform object, then:
    //     1. If types includes an interface type that V implements, then return the IDL value that is a reference to
    //        the object V.
    if value.is_object()
        && interface_names
            .iter()
            .any(|interface_name| implements_cpp_interface(value.as_object(), *interface_name))
    {
        return Ok(CppInterface(value.as_object()));
    }
    throw_no_union_types_matched(vm)
}

/// https://webidl.spec.whatwg.org/#js-interface
impl ToJs for CppInterface {
    fn to_js(self, _: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_object(self.0))
    }
}

/// An IDL sequence converted to a JS value, https://webidl.spec.whatwg.org/#js-sequence, with the conversion of its
/// elements.
pub(crate) fn sequence_to_js<T>(
    cx: &CallContext,
    sequence: Vec<T>,
    mut element_to_js: impl FnMut(T) -> ThrowCompletionOr<Value>,
) -> ThrowCompletionOr<Value> {
    // 1. Let n be the length of S.
    // 2. Let A be a new Array object created as if by the expression [].
    let array = Array::create(cx.vm(), cx.realm(), sequence.len() as u64, None)?;

    // 3. Initialize i to be 0.
    // 4. While i < n:
    for (index, element) in sequence.into_iter().enumerate() {
        // 1. Let V be the value in S at index i.
        // 2. Let E be the result of converting V to a JavaScript value.
        let element = element_to_js(element)?;

        // 3. Let P be the result of calling ! ToString(i).
        // 4. Perform ! CreateDataPropertyOrThrow(A, P, E).
        // 5. Set i to i + 1.
        let index = u32::try_from(index).expect("an array of the length of the sequence was created");
        array.create_data_property(cx.vm(), &PropertyKey::from(index), element, None, None)?;
    }

    // 5. Return A.
    Ok(Value::from_object(array))
}

/// sequence_to_js() for a sequence whose elements are cells, such as objects of interfaces. The conversion of an
/// element can allocate, and a Vec hides the elements after it from the collector, so the sequence is kept alive.
pub(crate) fn rooted_sequence_to_js<T: Trace + Clone + 'static>(
    cx: &CallContext,
    sequence: Vec<T>,
    mut element_to_js: impl FnMut(T) -> ThrowCompletionOr<Value>,
) -> ThrowCompletionOr<Value> {
    let sequence = Root::new(cx.vm(), sequence);
    let array = Array::create(cx.vm(), cx.realm(), sequence.get().len() as u64, None)?;
    for (index, element) in sequence.get().iter().enumerate() {
        let element = element_to_js(element.clone())?;
        let index = u32::try_from(index).expect("an array of the length of the sequence was created");
        array.create_data_property(cx.vm(), &PropertyKey::from(index), element, None, None)?;
    }
    Ok(Value::from_object(array))
}

/// An IDL record converted to a JS value, https://webidl.spec.whatwg.org/#js-record, with the conversion of its
/// values. The entries of the record are in the order of the Vec.
pub(crate) fn record_to_js<K: Into<Utf16String>, V>(
    cx: &CallContext,
    record: Vec<(K, V)>,
    mut value_to_js: impl FnMut(V) -> ThrowCompletionOr<Value>,
) -> ThrowCompletionOr<Value> {
    // 1. Let result be OrdinaryObjectCreate(%Object.prototype%).
    let result = create_dictionary_object(cx);

    // 2. For each key → value of D:
    for (key, value) in record {
        // 1. Let jsKey be key converted to a JavaScript value.
        let key = PropertyKey::from_utf16_string(&key.into());

        // 2. Let jsValue be value converted to a JavaScript value.
        let value = value_to_js(value)?;

        // 3. Let created be ! CreateDataProperty(result, jsKey, jsValue).
        // 4. Assert: created is true.
        result.create_data_property(cx.vm(), &key, value, None, None)?;
    }

    // 3. Return result.
    Ok(Value::from_object(result))
}
