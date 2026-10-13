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
    Array, ArrayBuffer, ArrayBufferOrder, DataView, ErrorKind, ErrorType, Gc, Object, PrimitiveString, PropertyKey,
    ThrowCompletionOr, TypedArrayBase, TypedArrayKind, Value, Vm, get_view_byte_length, is_typed_array_out_of_bounds,
    is_view_out_of_bounds, make_data_view_with_buffer_witness_record, make_typed_array_with_buffer_witness_record,
    to_well_formed, typed_array_byte_length,
};

use crate::bindings::CallContext;
use crate::bindings::interface::create_dictionary_object;

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
        Self(Utf16String::well_formed_from_utf16(code_units))
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

/// A JS value converted to a USVString, https://webidl.spec.whatwg.org/#es-USVString
pub(crate) fn to_usv_string(vm: &Vm, value: Value) -> ThrowCompletionOr<UsvString> {
    // 1. Let string be the result of converting V to a DOMString.
    let string = to_dom_string(vm, value)?;

    // 2. Return an IDL USVString value that is the result of converting string to a sequence of scalar values.
    Ok(UsvString(to_well_formed(&string)))
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
pub(crate) enum BufferSource {
    ArrayBuffer(Gc<ArrayBuffer>),
    DataView(Gc<DataView>),
    TypedArray(Gc<TypedArrayBase>),
}

impl BufferSource {
    /// Calls the function with the bytes that the buffer source holds, https://webidl.spec.whatwg.org/#dfn-get-buffer-source-copy,
    /// without copying them unless their buffer is shared. A detached buffer and a view that is out of bounds hold
    /// no bytes.
    ///
    /// # Safety
    ///
    /// The function must not run script or allocate a cell, either of which could detach, resize or free the buffer
    /// that the bytes are in.
    pub(crate) unsafe fn with_bytes<R>(&self, function: impl FnOnce(&[u8]) -> R) -> R {
        let (buffer, offset, length) = match self {
            Self::TypedArray(typed_array) => {
                let record = make_typed_array_with_buffer_witness_record(typed_array, ArrayBufferOrder::SeqCst);
                // AD-HOC: The WebIDL spec has not been updated for resizable ArrayBuffer objects. This check follows
                //         the behavior of step 7 of the TypedArray.prototype.buffer getter.
                if is_typed_array_out_of_bounds(&record) {
                    return function(&[]);
                }
                let length = typed_array_byte_length(&record) as usize;
                (
                    typed_array.viewed_array_buffer(),
                    typed_array.byte_offset() as usize,
                    length,
                )
            }
            Self::DataView(view) => {
                let record = make_data_view_with_buffer_witness_record(*view, ArrayBufferOrder::SeqCst);
                // AD-HOC: The WebIDL spec has not been updated for resizable ArrayBuffer objects. This check follows
                //         the behavior of step 7 of the DataView.prototype.buffer getter.
                if is_view_out_of_bounds(&record) {
                    return function(&[]);
                }
                let length = get_view_byte_length(&record) as usize;
                (view.viewed_array_buffer(), view.byte_offset() as usize, length)
            }
            Self::ArrayBuffer(buffer) => (*buffer, 0, buffer.byte_length()),
        };

        // 7. If IsDetachedBuffer(jsArrayBuffer) is true, then return the empty byte sequence.
        if buffer.is_detached() || length == 0 {
            return function(&[]);
        }

        // NB: Another thread can write the bytes of a shared buffer at any time, so the function gets a copy of them.
        if buffer.is_shared_array_buffer() {
            return function(&buffer.copy_to_byte_buffer(offset, length));
        }
        let bytes = buffer.with_data_block(|data_block| data_block.data_at(offset));
        assert!(!bytes.is_null());
        // SAFETY: The buffer is not detached and holds `length` bytes from `offset`, which nothing writes while the
        //         function runs, as the buffer is not shared and the caller keeps script from running.
        function(unsafe { core::slice::from_raw_parts(bytes, length) })
    }
}

/// A JS value converted to a BufferSource, with the steps of the conversion to a union type that apply to the buffer
/// source types, https://webidl.spec.whatwg.org/#js-union
pub(crate) fn to_buffer_source(vm: &Vm, value: Value, rules: &BufferSourceRules) -> ThrowCompletionOr<BufferSource> {
    if value.is_object() {
        let object = value.as_object();

        // 6. If V is an Object, V has an [[ArrayBufferData]] internal slot, and IsSharedArrayBuffer(V) is false, then:
        //     1. If types includes ArrayBuffer, then return the result of converting V to ArrayBuffer.
        if let Some(buffer) = object.downcast::<ArrayBuffer>()
            && !buffer.is_shared_array_buffer()
        {
            check_buffer_of_buffer_source(vm, &buffer, "ArrayBuffer", rules)?;
            return Ok(BufferSource::ArrayBuffer(buffer));
        }

        // FIXME: 7. If V is an Object, V has an [[ArrayBufferData]] internal slot, and IsSharedArrayBuffer(V) is true,
        //           then:

        // 8. If V is an Object and V has a [[DataView]] internal slot, then:
        //     1. If types includes DataView, then return the result of converting V to DataView.
        if let Some(view) = object.downcast::<DataView>() {
            check_buffer_of_buffer_source(vm, &view.viewed_array_buffer(), "DataView", rules)?;
            return Ok(BufferSource::DataView(view));
        }

        // 9. If V is an Object and V has a [[TypedArrayName]] internal slot, then:
        //     1. If types includes a typed array type whose name is the value of V's [[TypedArrayName]] internal slot,
        //        then return the result of converting V to that type.
        if let Some(typed_array) = object.downcast::<TypedArrayBase>() {
            let type_name = format!("{:?}", typed_array.kind());
            check_buffer_of_buffer_source(vm, &typed_array.viewed_array_buffer(), &type_name, rules)?;
            return Ok(BufferSource::TypedArray(typed_array));
        }
    }

    // 19. Throw a TypeError.
    vm.throw_completion_with_message(ErrorKind::TypeError, "No union types matched".to_string())
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
