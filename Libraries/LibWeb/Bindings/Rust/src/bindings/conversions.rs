/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Conversions between JS values and the Rust types of IDL values,
//! https://webidl.spec.whatwg.org/#es-type-mapping

use core::mem::MaybeUninit;

use ak::{Utf16String, Utf16StringUnits, convert_valid_utf16_to_utf8, utf8_length_from_valid_utf16};
use libjs_rust::api::{
    ErrorKind, ErrorType, Gc, PrimitiveString, ThrowCompletionOr, TypedArrayBase, TypedArrayKind, Value, Vm,
    to_well_formed,
};

use crate::bindings::CallContext;

/// A USVString, a string without lone surrogates. Its UTF-16 code units are always well-formed.
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

    pub(crate) fn into_utf16_string(self) -> Utf16String {
        self.0
    }
}

/// A JS value converted to a DOMString, https://webidl.spec.whatwg.org/#es-DOMString
pub(crate) fn to_dom_string(vm: &Vm, value: Value) -> ThrowCompletionOr<Utf16String> {
    value.to_utf16_string(vm)
}

/// A JS value converted to a USVString, https://webidl.spec.whatwg.org/#es-USVString
pub(crate) fn to_usv_string(vm: &Vm, value: Value) -> ThrowCompletionOr<UsvString> {
    // 1. Let string be the result of converting V to a DOMString.
    let string = to_dom_string(vm, value)?;

    // 2. Return an IDL USVString value that is the result of converting string to a sequence of scalar values.
    Ok(UsvString(to_well_formed(&string)))
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
    let buffer = typed_array.viewed_array_buffer();

    // 3. If the conversion is not to an IDL type associated with the [AllowShared] extended attribute, and
    //    IsSharedArrayBuffer(V.[[ViewedArrayBuffer]]) is true, then throw a TypeError.
    if !rules.allow_shared && buffer.is_shared_array_buffer() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::SharedArrayBuffer, &[]);
    }

    // 4. If the conversion is not to an IDL type associated with the [AllowResizable] extended attribute, and
    //    IsFixedLengthArrayBuffer(V.[[ViewedArrayBuffer]]) is false, then throw a TypeError.
    if !rules.allow_resizable && !buffer.is_fixed_length() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::NotAnObjectOfType,
            &[&format!("fixed-length {name}")],
        );
    }

    // 5. Return the IDL value of type T that is a reference to the same object as V.
    Ok(typed_array)
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
