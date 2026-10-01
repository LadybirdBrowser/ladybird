/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of Libraries/LibJS/Runtime/Value.cpp the runtime implements so far.

use core::fmt;
use core::ptr::NonNull;

use ak::Utf16String;

use crate::build_configuration::HEAP_REGION_OFFSET_MASK;
use crate::bytecode::executable::{PropertyLookupCache, StaticPropertyLookupCacheSite};
use crate::bytecode::property_access::{CachePropertyAbsence, GetByIdMode, get_by_id};
use crate::gc::capi::js_heap_region_base;
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::primitive_string::PrimitiveString;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::accessor::Accessor;
use crate::runtime::big_int::BigInt;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::object::PropertyLookupPhase;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::symbol::Symbol;
use crate::runtime::value_conversions::{self, string_to_number};
use crate::utf16::Utf16View;
use libjs_abi::value as nan_box;

/// Makes the whole encoded value live up to this point. The conservative stack scan recognizes a cell by its tagged
/// value or by its address, but not by its bare heap offset, which the compiler could otherwise keep instead of the
/// value while the value is held across an allocation.
#[inline(always)]
fn keep_encoded_value_alive(encoded: u64) {
    // SAFETY: The assembly is empty: it only has the compiler put `encoded` in a register here.
    unsafe { core::arch::asm!("/* {0} */", in(reg) encoded, options(nomem, nostack, preserves_flags)) };
}

impl Value {
    pub const fn from_bool(value: bool) -> Self {
        if value { Self::TRUE } else { Self::FALSE }
    }

    pub const fn from_i32(value: i32) -> Self {
        Self(nan_box::SHIFTED_INT32_TAG | value as u32 as u64)
    }

    /// Like the C++ Value(double): integral doubles that fit in an i32, other than negative zero, are stored as
    /// Int32, and every NaN becomes the canonical NaN.
    pub fn from_f64(value: f64) -> Self {
        let is_negative_zero = value.to_bits() == nan_box::NEGATIVE_ZERO_BITS;
        if value >= f64::from(i32::MIN) && value <= f64::from(i32::MAX) && value.trunc() == value && !is_negative_zero {
            return Self::from_i32(value as i32);
        }
        if value.is_nan() {
            return Self(nan_box::CANON_NAN_BITS);
        }
        Self(value.to_bits())
    }

    pub const fn tag(self) -> u64 {
        self.0 >> nan_box::TAG_SHIFT
    }

    pub const fn is_empty(self) -> bool {
        self.0 == nan_box::EMPTY_VALUE
    }

    pub const fn is_undefined(self) -> bool {
        self.0 == nan_box::UNDEFINED_VALUE
    }

    pub const fn is_null(self) -> bool {
        self.0 == nan_box::NULL_VALUE
    }

    pub const fn is_nullish(self) -> bool {
        (self.tag() & nan_box::IS_NULLISH_EXTRACT_PATTERN) == nan_box::IS_NULLISH_PATTERN
    }

    pub const fn is_boolean(self) -> bool {
        self.tag() == nan_box::BOOLEAN_TAG
    }

    pub const fn is_int32(self) -> bool {
        self.tag() == nan_box::INT32_TAG
    }

    pub const fn is_double(self) -> bool {
        (self.0 & nan_box::CANON_NAN_BITS) != nan_box::CANON_NAN_BITS || self.0 == nan_box::CANON_NAN_BITS
    }

    pub const fn is_number(self) -> bool {
        self.is_double() || self.is_int32()
    }

    pub const fn is_cell(self) -> bool {
        (self.tag() & nan_box::IS_CELL_PATTERN) == nan_box::IS_CELL_PATTERN
    }

    pub const fn is_object(self) -> bool {
        self.tag() == nan_box::OBJECT_TAG
    }

    pub const fn is_string(self) -> bool {
        self.tag() == nan_box::STRING_TAG
    }

    pub const fn is_symbol(self) -> bool {
        self.tag() == nan_box::SYMBOL_TAG
    }

    pub const fn is_bigint(self) -> bool {
        self.tag() == nan_box::BIGINT_TAG
    }

    pub const fn is_accessor(self) -> bool {
        self.tag() == nan_box::ACCESSOR_TAG
    }

    pub fn as_bool(self) -> bool {
        assert!(self.is_boolean());
        self.0 & 1 != 0
    }

    pub fn as_i32(self) -> i32 {
        debug_assert!(self.is_int32());
        self.0 as u32 as i32
    }

    /// The numeric value of a number.
    pub fn as_f64(self) -> f64 {
        debug_assert!(self.is_number());
        if self.is_int32() {
            return f64::from(self.as_i32());
        }
        f64::from_bits(self.0)
    }

    pub fn is_integral_number(self) -> bool {
        if self.is_int32() {
            return true;
        }
        self.is_finite_number() && self.as_f64().trunc() == self.as_f64()
    }

    pub fn is_finite_number(self) -> bool {
        if !self.is_number() {
            return false;
        }
        if self.is_int32() {
            return true;
        }
        self.as_f64().is_finite()
    }

    pub(crate) fn with_cell_tag<T>(tag: u64, cell: Gc<T>) -> Self {
        let address = cell.as_ptr() as usize as u64;
        Self((tag << nan_box::TAG_SHIFT) | (address & HEAP_REGION_OFFSET_MASK))
    }

    /// # Safety
    ///
    /// The value must hold a cell of type T.
    pub(crate) unsafe fn cell<T>(self) -> Gc<T> {
        debug_assert!(self.is_cell());
        keep_encoded_value_alive(self.0);
        // SAFETY: Cell values are offsets into the heap region, whose base LibGC fixed before any cell existed.
        let base = unsafe { js_heap_region_base } as u64;
        let address = (base + (self.0 & HEAP_REGION_OFFSET_MASK)) as usize;
        // SAFETY: The caller guarantees the value holds a live cell of type T.
        unsafe { Gc::from_non_null(NonNull::new_unchecked(core::ptr::without_provenance_mut::<T>(address))) }
    }

    pub fn from_object<T>(object: Gc<T>) -> Self {
        Self::with_cell_tag(nan_box::OBJECT_TAG, object)
    }

    pub fn from_string(string: Gc<PrimitiveString>) -> Self {
        Self::with_cell_tag(nan_box::STRING_TAG, string)
    }

    pub fn from_symbol(symbol: Gc<Symbol>) -> Self {
        Self::with_cell_tag(nan_box::SYMBOL_TAG, symbol)
    }

    pub fn from_bigint(bigint: Gc<BigInt>) -> Self {
        Self::with_cell_tag(nan_box::BIGINT_TAG, bigint)
    }

    pub fn from_accessor(accessor: Gc<Accessor>) -> Self {
        Self::with_cell_tag(nan_box::ACCESSOR_TAG, accessor)
    }

    pub fn as_object(self) -> Gc<Object> {
        assert!(self.is_object());
        // SAFETY: The tag says the value holds an object.
        unsafe { self.cell() }
    }

    pub fn as_string(self) -> Gc<PrimitiveString> {
        assert!(self.is_string());
        // SAFETY: The tag says the value holds a string.
        unsafe { self.cell() }
    }

    pub fn as_symbol(self) -> Gc<Symbol> {
        assert!(self.is_symbol());
        // SAFETY: The tag says the value holds a symbol.
        unsafe { self.cell() }
    }

    pub fn as_bigint(self) -> Gc<BigInt> {
        assert!(self.is_bigint());
        // SAFETY: The tag says the value holds a BigInt.
        unsafe { self.cell() }
    }

    pub fn as_accessor(self) -> Gc<Accessor> {
        assert!(self.is_accessor());
        // SAFETY: The tag says the value holds an accessor.
        unsafe { self.cell() }
    }

    pub fn is_nan(self) -> bool {
        self.is_number() && self.as_f64().is_nan()
    }

    pub fn is_infinity(self) -> bool {
        self.is_number() && self.as_f64().is_infinite()
    }

    pub fn is_positive_zero(self) -> bool {
        self.is_number() && self.as_f64().to_bits() == 0
    }

    pub fn is_negative_zero(self) -> bool {
        self.is_double() && self.0 == nan_box::NEGATIVE_ZERO_BITS
    }

    // 7.2.3 IsCallable ( argument ), https://tc39.es/ecma262/#sec-iscallable
    pub fn is_function(self) -> bool {
        // 1. If argument is not an Object, return false.
        // 2. If argument has a [[Call]] internal method, return true.
        // 3. Return false.
        self.is_object() && self.as_object().is_function()
    }

    pub fn as_function(self) -> Gc<FunctionObject> {
        assert!(self.is_function());
        // SAFETY: The object has the IsFunction flag, which only function objects set.
        unsafe { self.cell() }
    }

    // 7.2.4 IsConstructor ( argument ), https://tc39.es/ecma262/#sec-isconstructor
    pub fn is_constructor(self) -> bool {
        // 1. If Type(argument) is not Object, return false.
        if !self.is_function() {
            return false;
        }

        // 2. If argument has a [[Construct]] internal method, return true.
        // 3. Return false.
        self.as_object().has_constructor()
    }

    // 7.1.2 ToBoolean ( argument ), https://tc39.es/ecma262/#sec-toboolean
    #[inline]
    pub fn to_boolean(self) -> bool {
        if self.is_boolean() {
            return self.as_bool();
        }
        self.to_boolean_slow_case()
    }

    #[inline(never)]
    fn to_boolean_slow_case(self) -> bool {
        if self.is_double() {
            if self.is_nan() {
                return false;
            }
            return self.as_f64() != 0.0;
        }

        match self.tag() {
            // 1. If argument is a Boolean, return argument.
            nan_box::BOOLEAN_TAG => self.as_bool(),
            // 2. If argument is any of undefined, null, +0𝔽, -0𝔽, NaN, 0ℤ, or the empty String, return false.
            nan_box::UNDEFINED_TAG | nan_box::NULL_TAG => false,
            nan_box::INT32_TAG => self.as_i32() != 0,
            nan_box::STRING_TAG => !self.as_string().is_empty(),
            nan_box::BIGINT_TAG => !num_traits::Zero::is_zero(self.as_bigint().big_integer()),
            nan_box::OBJECT_TAG => {
                // B.3.6.1 Changes to ToBoolean, https://tc39.es/ecma262/#sec-IsHTMLDDA-internal-slot-to-boolean
                // 3. If argument is an Object and argument has an [[IsHTMLDDA]] internal slot, return false.
                // 4. Return true.
                !self.as_object().is_htmldda()
            }
            nan_box::SYMBOL_TAG => true,
            _ => unreachable!("ToBoolean of a value that is not a language value"),
        }
    }

    // 7.1.1 ToPrimitive ( input [ , preferredType ] ), https://tc39.es/ecma262/#sec-toprimitive
    #[inline]
    pub fn to_primitive(self, vm: &Vm, preferred_type: PreferredType) -> ThrowCompletionOr<Value> {
        if !self.is_object() {
            return Ok(self);
        }
        self.to_primitive_slow_case(vm, preferred_type)
    }

    #[inline(never)]
    fn to_primitive_slow_case(self, vm: &Vm, mut preferred_type: PreferredType) -> ThrowCompletionOr<Value> {
        // 1. If input is an Object, then
        if self.is_object() {
            // a. Let exoticToPrim be ? GetMethod(input, @@toPrimitive).
            let exotic_to_primitive = self.get_method_with_cache(
                vm,
                &PropertyKey::from(vm.well_known_symbols().to_primitive),
                vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::ValueToPrimitive),
            )?;

            // b. If exoticToPrim is not undefined, then
            if let Some(exotic_to_primitive) = exotic_to_primitive {
                let hint = match preferred_type {
                    // i. If preferredType is not present, let hint be "default".
                    PreferredType::Default => "default",
                    // ii. Else if preferredType is string, let hint be "string".
                    PreferredType::String => "string",
                    // iii. Else,
                    // 1. Assert: preferredType is number.
                    // 2. Let hint be "number".
                    PreferredType::Number => "number",
                };

                // iv. Let result be ? Call(exoticToPrim, input, « hint »).
                let hint_string = Value::from_string(PrimitiveString::create_from_utf8(vm, hint));
                let result = call_function_object(vm, exotic_to_primitive, self, &[hint_string])?;

                // v. If result is not an Object, return result.
                if !result.is_object() {
                    return Ok(result);
                }

                // vi. Throw a TypeError exception.
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::ToPrimitiveReturnedObject,
                    &[&self, &hint],
                );
            }

            // c. If preferredType is not present, let preferredType be number.
            if preferred_type == PreferredType::Default {
                preferred_type = PreferredType::Number;
            }

            // d. Return ? OrdinaryToPrimitive(input, preferredType).
            return self.as_object().ordinary_to_primitive(vm, preferred_type);
        }

        // 2. Return input.
        Ok(self)
    }

    // 7.1.18 ToObject ( argument ), https://tc39.es/ecma262/#sec-toobject
    #[inline]
    pub fn to_object(self, vm: &Vm) -> ThrowCompletionOr<Gc<Object>> {
        if self.is_object() {
            return Ok(self.as_object());
        }
        self.to_object_slow(vm)
    }

    #[inline(never)]
    fn to_object_slow(self, vm: &Vm) -> ThrowCompletionOr<Gc<Object>> {
        assert!(!self.is_empty());

        // Undefined
        // Null
        if self.is_nullish() {
            // Throw a TypeError exception.
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ToObjectNullOrUndefined, &[]);
        }

        // Number, Boolean, String, Symbol, BigInt
        // Return a new wrapper object whose internal slot is set to argument. See 20.3, 20.4, 21.1, 21.2 and 22.1.
        unimplemented_runtime_function("ToObject of a primitive, which needs the wrapper object classes", 0)
    }

    // 7.1.4 ToNumber ( argument ), https://tc39.es/ecma262/#sec-tonumber
    #[inline]
    pub fn to_number(self, vm: &Vm) -> ThrowCompletionOr<Value> {
        if self.is_number() {
            return Ok(self);
        }
        self.to_number_slow_case(vm)
    }

    #[inline(never)]
    fn to_number_slow_case(self, vm: &Vm) -> ThrowCompletionOr<Value> {
        assert!(!self.is_empty());

        // 1. If argument is a Number, return argument.
        if self.is_number() {
            return Ok(self);
        }

        match self.tag() {
            // 2. If argument is either a Symbol or a BigInt, throw a TypeError exception.
            nan_box::SYMBOL_TAG => {
                vm.throw_completion(ErrorKind::TypeError, ErrorType::Convert, &[&"symbol", &"number"])
            }
            nan_box::BIGINT_TAG => {
                vm.throw_completion(ErrorKind::TypeError, ErrorType::Convert, &[&"BigInt", &"number"])
            }
            // 3. If argument is undefined, return NaN.
            nan_box::UNDEFINED_TAG => Ok(Value::from_f64(f64::NAN)),
            // 4. If argument is either null or false, return +0𝔽.
            nan_box::NULL_TAG => Ok(Value::from_i32(0)),
            // 5. If argument is true, return 1𝔽.
            nan_box::BOOLEAN_TAG => Ok(Value::from_i32(i32::from(self.as_bool()))),
            // 6. If argument is a String, return StringToNumber(argument).
            nan_box::STRING_TAG => Ok(Value::from_f64(
                self.as_string()
                    .utf16_string_view()
                    .with_utf16_code_units(string_to_number),
            )),
            // 7. Assert: argument is an Object.
            nan_box::OBJECT_TAG => {
                // 8. Let primValue be ? ToPrimitive(argument, number).
                let primitive_value = self.to_primitive(vm, PreferredType::Number)?;

                // 9. Assert: primValue is not an Object.
                assert!(!primitive_value.is_object());

                // 10. Return ? ToNumber(primValue).
                primitive_value.to_number(vm)
            }
            _ => unreachable!("ToNumber of a value that is not a language value"),
        }
    }

    // 7.1.6 ToInt32 ( argument ), https://tc39.es/ecma262/#sec-toint32
    #[inline]
    pub fn to_i32(self, vm: &Vm) -> ThrowCompletionOr<i32> {
        if self.is_int32() {
            return Ok(self.as_i32());
        }
        self.to_i32_slow_case(vm)
    }

    #[inline(never)]
    fn to_i32_slow_case(self, vm: &Vm) -> ThrowCompletionOr<i32> {
        assert!(!self.is_int32());

        // 1. Let number be ? ToNumber(argument).
        let number = self.to_number(vm)?.as_f64();

        // 2-5.
        Ok(value_conversions::to_i32(number))
    }

    // 7.1.7 ToUint32 ( argument ), https://tc39.es/ecma262/#sec-touint32
    #[inline]
    pub fn to_u32(self, vm: &Vm) -> ThrowCompletionOr<u32> {
        Ok(self.to_i32(vm)? as u32)
    }

    // 7.1.5 ToIntegerOrInfinity ( argument ), https://tc39.es/ecma262/#sec-tointegerorinfinity
    pub fn to_integer_or_infinity(self, vm: &Vm) -> ThrowCompletionOr<f64> {
        // 1. Let number be ? ToNumber(argument).
        let number = self.to_number(vm)?;

        // 2-7.
        Ok(value_conversions::to_integer_or_infinity(number.as_f64()))
    }

    // 7.1.20 ToLength ( argument ), https://tc39.es/ecma262/#sec-tolength
    pub fn to_length(self, vm: &Vm) -> ThrowCompletionOr<u64> {
        // 1. Let len be ? ToIntegerOrInfinity(argument).
        let len = self.to_integer_or_infinity(vm)?;

        // 2. If len ≤ 0, return +0𝔽.
        // 3. Return 𝔽(min(len, 2^53 - 1)).
        Ok(value_conversions::to_length(len))
    }

    // 13.5.3 The typeof Operator, https://tc39.es/ecma262/#sec-typeof-operator
    pub fn typeof_(self, vm: &Vm) -> Gc<PrimitiveString> {
        let cached_strings = vm.cached_strings();

        // 9. If val is a Number, return "number".
        if self.is_number() {
            return cached_strings.number;
        }

        match self.tag() {
            // 4. If val is undefined, return "undefined".
            nan_box::UNDEFINED_TAG => cached_strings.undefined,
            // 5. If val is null, return "object".
            nan_box::NULL_TAG => cached_strings.object,
            // 6. If val is a String, return "string".
            nan_box::STRING_TAG => cached_strings.string,
            // 7. If val is a Symbol, return "symbol".
            nan_box::SYMBOL_TAG => cached_strings.symbol,
            // 8. If val is a Boolean, return "boolean".
            nan_box::BOOLEAN_TAG => cached_strings.boolean,
            // 10. If val is a BigInt, return "bigint".
            nan_box::BIGINT_TAG => cached_strings.bigint,
            // 11. Assert: val is an Object.
            nan_box::OBJECT_TAG => {
                // B.3.6.3 Changes to the typeof Operator, https://tc39.es/ecma262/#sec-IsHTMLDDA-internal-slot-typeof
                // 12. If val has an [[IsHTMLDDA]] internal slot, return "undefined".
                if self.as_object().is_htmldda() {
                    return cached_strings.undefined;
                }
                // 13. If val has a [[Call]] internal slot, return "function".
                if self.is_function() {
                    return cached_strings.function;
                }
                // 14. Return "object".
                cached_strings.object
            }
            _ => unreachable!("typeof of a value that is not a language value"),
        }
    }

    pub fn to_utf16_string_without_side_effects(self) -> Utf16String {
        if self.is_double() {
            return Utf16String::from_utf16(&number_to_utf16_string(self.as_f64()));
        }

        match self.tag() {
            nan_box::UNDEFINED_TAG => Utf16String::from_utf8("undefined"),
            nan_box::NULL_TAG => Utf16String::from_utf8("null"),
            nan_box::BOOLEAN_TAG => Utf16String::from_utf8(if self.as_bool() { "true" } else { "false" }),
            nan_box::INT32_TAG => Utf16String::from_utf8(&self.as_i32().to_string()),
            nan_box::STRING_TAG => self.as_string().utf16_string(),
            nan_box::SYMBOL_TAG => self.as_symbol().descriptive_string(),
            nan_box::BIGINT_TAG => self.as_bigint().to_utf16_string(),
            nan_box::OBJECT_TAG => Utf16String::from_utf8(&format!("[object {}]", self.as_object().class().name)),
            nan_box::ACCESSOR_TAG => Utf16String::from_utf8("<accessor>"),
            nan_box::EMPTY_TAG => Utf16String::from_utf8("<empty>"),
            _ => unreachable!("a value with an unknown tag"),
        }
    }

    // 7.3.3 GetV ( V, P ), https://tc39.es/ecma262/#sec-getv
    pub fn get(self, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? ToObject(V).
        let object = {
            // OPTIMIZATION: For primitive values, we can skip allocation of temporary object and look
            //               directly into prototype where requested property is located.
            let current_realm = || vm.current_realm().expect("there is a current realm");
            if self.is_string() && *property_key != vm.names.length {
                current_realm().string_prototype()
            } else if self.is_boolean() {
                current_realm().boolean_prototype()
            } else if self.is_number() {
                current_realm().number_prototype()
            } else if self.is_bigint() {
                current_realm().bigint_prototype()
            } else if self.is_symbol() {
                current_realm().symbol_prototype()
            } else {
                self.to_object(vm)?
            }
        };

        // 2. Return ? O.[[Get]](P, V).
        object.internal_get(vm, property_key, self, None, PropertyLookupPhase::OwnProperty)
    }

    pub fn get_with_cache(
        self,
        vm: &Vm,
        property_key: &PropertyKey,
        cache: &PropertyLookupCache,
    ) -> ThrowCompletionOr<Value> {
        if self.is_nullish() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ToObjectNullOrUndefined, &[]);
        }
        get_by_id(
            vm,
            GetByIdMode::Normal,
            || None,
            property_key,
            self,
            self,
            cache,
            CachePropertyAbsence::Yes,
        )
    }

    // 7.3.11 GetMethod ( V, P ), https://tc39.es/ecma262/#sec-getmethod
    pub fn get_method(self, vm: &Vm, property_key: &PropertyKey) -> ThrowCompletionOr<Option<Gc<FunctionObject>>> {
        // 1. Let func be ? GetV(V, P).
        let function = self.get(vm, property_key)?;
        Self::finish_get_method(vm, function)
    }

    // 7.3.11 GetMethod ( V, P ), https://tc39.es/ecma262/#sec-getmethod
    pub fn get_method_with_cache(
        self,
        vm: &Vm,
        property_key: &PropertyKey,
        cache: &PropertyLookupCache,
    ) -> ThrowCompletionOr<Option<Gc<FunctionObject>>> {
        // 1. Let func be ? GetV(V, P).
        let function = self.get_with_cache(vm, property_key, cache)?;
        Self::finish_get_method(vm, function)
    }

    fn finish_get_method(vm: &Vm, function: Value) -> ThrowCompletionOr<Option<Gc<FunctionObject>>> {
        // 2. If func is either undefined or null, return undefined.
        if function.is_nullish() {
            return Ok(None);
        }

        // 3. If IsCallable(func) is false, throw a TypeError exception.
        if !function.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&function]);
        }

        // 4. Return func.
        Ok(Some(function.as_function()))
    }
}

/// Mirrors JS::Value::PreferredType.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PreferredType {
    Default,
    String,
    Number,
}

fn same_type_for_equality(lhs: Value, rhs: Value) -> bool {
    // If the top two bytes are identical then either:
    // both are NaN boxed Values with the same type
    // or they are doubles which happen to have the same top bytes.
    if (lhs.0 & nan_box::TAG_EXTRACTION) == (rhs.0 & nan_box::TAG_EXTRACTION) {
        return true;
    }

    if lhs.is_number() && rhs.is_number() {
        return true;
    }

    // One of the Values is not a number and they do not have the same tag
    false
}

// 7.2.10 SameValue ( x, y ), https://tc39.es/ecma262/#sec-samevalue
pub fn same_value(lhs: Value, rhs: Value) -> bool {
    // 1. If Type(x) is different from Type(y), return false.
    if !same_type_for_equality(lhs, rhs) {
        return false;
    }

    // 2. If x is a Number, then
    if lhs.is_number() {
        // a. Return Number::sameValue(x, y).

        // 6.1.6.1.14 Number::sameValue ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-sameValue
        // 1. If x is NaN and y is NaN, return true.
        if lhs.is_nan() && rhs.is_nan() {
            return true;
        }
        // 2. If x is +0𝔽 and y is -0𝔽, return false.
        if lhs.is_positive_zero() && rhs.is_negative_zero() {
            return false;
        }
        // 3. If x is -0𝔽 and y is +0𝔽, return false.
        if lhs.is_negative_zero() && rhs.is_positive_zero() {
            return false;
        }
        // 4. If x is the same Number value as y, return true.
        // 5. Return false.
        return lhs.as_f64() == rhs.as_f64();
    }

    // 3. Return SameValueNonNumber(x, y).
    same_value_non_number(lhs, rhs)
}

// 7.2.11 SameValueZero ( x, y ), https://tc39.es/ecma262/#sec-samevaluezero
pub fn same_value_zero(lhs: Value, rhs: Value) -> bool {
    // 1. If Type(x) is different from Type(y), return false.
    if !same_type_for_equality(lhs, rhs) {
        return false;
    }

    // 2. If x is a Number, then
    if lhs.is_number() {
        // a. Return Number::sameValueZero(x, y).
        if lhs.is_nan() && rhs.is_nan() {
            return true;
        }
        return lhs.as_f64() == rhs.as_f64();
    }

    // 3. Return SameValueNonNumber(x, y).
    same_value_non_number(lhs, rhs)
}

// 7.2.12 SameValueNonNumber ( x, y ), https://tc39.es/ecma262/#sec-samevaluenonnumeric
pub fn same_value_non_number(lhs: Value, rhs: Value) -> bool {
    // 1. Assert: Type(x) is the same as Type(y).
    assert!(same_type_for_equality(lhs, rhs));
    assert!(!lhs.is_number());

    // 2. If x is a BigInt, then
    if lhs.is_bigint() {
        // a. Return BigInt::equal(x, y).

        // 6.1.6.2.13 BigInt::equal ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-equal
        // 1. If ℝ(x) = ℝ(y), return true; otherwise return false.
        return lhs.as_bigint().big_integer() == rhs.as_bigint().big_integer();
    }

    // 5. If x is a String, then
    if lhs.is_string() {
        // a. If x and y are exactly the same sequence of code units (same length and same code units at corresponding indices), return true; otherwise, return false.
        return *lhs.as_string() == *rhs.as_string();
    }

    // 3. If x is undefined, return true.
    // 4. If x is null, return true.
    // 6. If x is a Boolean, then
    //    a. If x and y are both true or both false, return true; otherwise, return false.
    // 7. If x is a Symbol, then
    //    a. If x and y are both the same Symbol value, return true; otherwise, return false.
    // 8. If x and y are the same Object value, return true. Otherwise, return false.
    // NOTE: All the options above will have the exact same bit representation in Value, so we can directly compare the bits.
    lhs.0 == rhs.0
}

/// Formats a value the way AK formats a C++ JS::Value, without side effects.
impl fmt::Display for Value {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let string = self.to_utf16_string_without_side_effects();
        formatter.write_str(&Utf16View::of_string(&string).to_utf8())
    }
}

impl core::fmt::Debug for Value {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(formatter, "Value(0x{:016x})", self.0)
    }
}

/// The text and UTF-16 builders Number::toString writes its ASCII output into.
pub trait NumberStringBuilder {
    fn append_ascii(&mut self, text: &[u8]);
    fn append_repeated_ascii(&mut self, code_unit: u8, count: usize);
}

impl NumberStringBuilder for String {
    fn append_ascii(&mut self, text: &[u8]) {
        self.extend(text.iter().copied().map(char::from));
    }

    fn append_repeated_ascii(&mut self, code_unit: u8, count: usize) {
        self.extend(core::iter::repeat_n(char::from(code_unit), count));
    }
}

impl NumberStringBuilder for Vec<u16> {
    fn append_ascii(&mut self, text: &[u8]) {
        self.extend(text.iter().copied().map(u16::from));
    }

    fn append_repeated_ascii(&mut self, code_unit: u8, count: usize) {
        self.extend(core::iter::repeat_n(u16::from(code_unit), count));
    }
}

pub fn number_to_string(value: f64) -> String {
    let mut builder = String::new();
    append_number_to_string(&mut builder, value);
    builder
}

pub fn number_to_utf16_string(value: f64) -> Vec<u16> {
    let mut builder = Vec::new();
    append_number_to_string(&mut builder, value);
    builder
}

// 6.1.6.1.20 Number::toString ( x ), https://tc39.es/ecma262/#sec-numeric-types-number-tostring
// Implementation for radix = 10
pub fn append_number_to_string(builder: &mut impl NumberStringBuilder, value: f64) {
    // 1. If x is NaN, return "NaN".
    if value.is_nan() {
        builder.append_ascii(b"NaN");
        return;
    }

    // 2. If x is +0𝔽 or -0𝔽, return "0".
    if value == 0.0 {
        builder.append_ascii(b"0");
        return;
    }

    // 4. If x is +∞𝔽, return "Infinity".
    if value.is_infinite() {
        builder.append_ascii(if value > 0.0 { b"Infinity" } else { b"-Infinity" });
        return;
    }

    // 5. Let n, k, and s be integers such that k ≥ 1, radix ^ (k - 1) ≤ s < radix ^ k, 𝔽(s × radix ^ (n - k)) is x,
    //    and k is as small as possible.
    let ak::DecimalExponentialForm {
        sign: is_negative,
        fraction: significand,
        exponent,
    } = ak::convert_to_decimal_exponential_form(value);
    let significand_digits = DecimalDigits::new(significand);
    let digits = significand_digits.as_bytes();
    let k = digits.len() as i32;
    let n = exponent + k;

    // 3. If x < -0𝔽, return the string-concatenation of "-" and Number::toString(-x, radix).
    if is_negative {
        builder.append_ascii(b"-");
    }

    // 6. If radix ≠ 10 or n is in the inclusive interval from -5 to 21, then
    if (-5..=21).contains(&n) {
        if n >= k {
            // a. If n ≥ k, return the k digits of s followed by n - k zeros.
            builder.append_ascii(digits);
            builder.append_repeated_ascii(b'0', (n - k) as usize);
        } else if n > 0 {
            // b. Else if n > 0, return the most significant n digits of s, ".", and the remaining k - n digits.
            builder.append_ascii(&digits[..n as usize]);
            builder.append_ascii(b".");
            builder.append_ascii(&digits[n as usize..]);
        } else {
            // c. Else, return "0.", -n zeros, and the k digits of s.
            builder.append_ascii(b"0.");
            builder.append_repeated_ascii(b'0', n.unsigned_abs() as usize);
            builder.append_ascii(digits);
        }
        return;
    }

    // 7. NOTE: In this case, the input will be represented using scientific E notation, such as 1.2e+3.
    // 9. If n < 0, let exponentSign be "-". 10. Else, let exponentSign be "+".
    let exponent_sign: &[u8] = if n < 0 { b"-" } else { b"+" };
    let exponent_digits = DecimalDigits::new(u64::from((n - 1).unsigned_abs()));

    // 11. If k is 1, return the single digit of s, "e", exponentSign, and the decimal representation of abs(n - 1).
    // 12. Return the most significant digit of s, ".", the remaining k - 1 digits, "e", exponentSign, and abs(n - 1).
    builder.append_ascii(&digits[..1]);
    if k > 1 {
        builder.append_ascii(b".");
        builder.append_ascii(&digits[1..]);
    }
    builder.append_ascii(b"e");
    builder.append_ascii(exponent_sign);
    builder.append_ascii(exponent_digits.as_bytes());
}

struct DecimalDigits {
    digits: [u8; 20],
    start: usize,
}

impl DecimalDigits {
    fn new(mut value: u64) -> Self {
        let mut digits = [0; 20];
        let mut start = digits.len();
        loop {
            start -= 1;
            digits[start] = b'0' + (value % 10) as u8;
            value /= 10;
            if value == 0 {
                break;
            }
        }
        Self { digits, start }
    }

    fn as_bytes(&self) -> &[u8] {
        &self.digits[self.start..]
    }
}
