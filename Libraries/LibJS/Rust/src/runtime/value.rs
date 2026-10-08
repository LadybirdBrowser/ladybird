/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The conversions and operators on values.

use core::ptr::NonNull;

use ak::Utf16String;
use num_traits::Zero;

use crate::build_configuration::HEAP_REGION_OFFSET_MASK;
use crate::bytecode::executable::{PropertyLookupCache, StaticPropertyLookupCacheSite};
use crate::bytecode::property_access::{CachePropertyAbsence, GetByIdMode, get_by_id};
use crate::gc::capi::js_heap_region_base;
use crate::gc::class_id::ClassId;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::primitive_string::PrimitiveString;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{call, call_function_object};
use crate::runtime::accessor::Accessor;
use crate::runtime::array::Array;
use crate::runtime::big_int::{BigInt, SignedBigInteger};
use crate::runtime::big_int_algorithms::{self, CompareResult};
use crate::runtime::big_int_object::BigIntObject;
use crate::runtime::boolean_object::BooleanObject;
use crate::runtime::bound_function::BoundFunction;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::number_object::NumberObject;
use crate::runtime::object::PropertyLookupPhase;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::proxy_object::ProxyObject;
use crate::runtime::string_object::StringObject;
use crate::runtime::symbol::Symbol;
use crate::runtime::symbol_object::SymbolObject;
use crate::runtime::value_conversions::{self, string_to_number};
use crate::utf16::{Utf16Display, Utf16StringBuilder, Utf16View};
use libjs_abi::Builtin;
use libjs_abi::value as nan_box;

/// Makes the whole encoded value live up to this point, and the decoded address from this point on. The conservative
/// stack scan recognizes a cell by its tagged value or by its address, but not by its bare heap offset, which the
/// compiler could otherwise keep instead of the value or the address while either is held across an allocation.
#[inline(always)]
fn decode_cell_address(encoded: u64) -> usize {
    // SAFETY: Cell values are offsets into the heap region, whose base LibGC fixed before any cell existed.
    let base = unsafe { js_heap_region_base } as u64;
    let mut address = base + (encoded & HEAP_REGION_OFFSET_MASK);
    // SAFETY: The assembly is empty: it only has the compiler put `encoded` and `address` in registers here.
    unsafe {
        core::arch::asm!(
            "/* {encoded} {address} */",
            encoded = in(reg) encoded,
            address = inout(reg) address,
            options(nomem, nostack, preserves_flags),
        );
    }
    address as usize
}

impl Value {
    pub const fn from_bool(value: bool) -> Self {
        if value { Self::TRUE } else { Self::FALSE }
    }

    pub const fn from_i32(value: i32) -> Self {
        Self(nan_box::SHIFTED_INT32_TAG | value as u32 as u64)
    }

    /// Integral doubles that fit in an i32, other than negative zero, are stored as Int32, and every NaN becomes the
    /// canonical NaN.
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
        let address = decode_cell_address(self.0);
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

    pub fn as_cell(self) -> Gc<CellHeader> {
        assert!(self.is_cell());
        // SAFETY: The tag says the value holds a cell, and every cell starts with its header.
        unsafe { self.cell() }
    }

    pub fn is_nan(self) -> bool {
        self.0 == nan_box::CANON_NAN_BITS
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

    pub const fn is_positive_infinity(self) -> bool {
        self.0 == nan_box::POSITIVE_INFINITY_BITS
    }

    pub const fn is_negative_infinity(self) -> bool {
        self.0 == nan_box::NEGATIVE_INFINITY_BITS
    }

    pub const fn is_non_negative_int32(self) -> bool {
        (self.0 & (nan_box::TAG_EXTRACTION | 0x8000_0000)) == nan_box::SHIFTED_INT32_TAG
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

    // 7.2.2 IsArray ( argument ), https://tc39.es/ecma262/#sec-isarray
    pub fn is_array(self, vm: &Vm) -> ThrowCompletionOr<bool> {
        // 1. If argument is not an Object, return false.
        if !self.is_object() {
            return Ok(false);
        }

        let object = self.as_object();

        // 2. If argument is an Array exotic object, return true.
        if object.is::<Array>() {
            return Ok(true);
        }

        // 3. If argument is a Proxy exotic object, then
        if let Some(proxy) = object.downcast::<ProxyObject>() {
            // a. Perform ? ValidateNonRevokedProxy(argument).
            proxy.validate_non_revoked_proxy(vm)?;

            // b. Let proxyTarget be argument.[[ProxyTarget]].
            let proxy_target = proxy.target();

            // c. Return ? IsArray(proxyTarget).
            return Value::from_object(proxy_target).is_array(vm);
        }

        // 4. Return false.
        Ok(false)
    }

    // 7.2.8 IsRegExp ( argument ), https://tc39.es/ecma262/#sec-isregexp
    pub fn is_regexp(self, vm: &Vm) -> ThrowCompletionOr<bool> {
        // 1. If argument is not an Object, return false.
        if !self.is_object() {
            return Ok(false);
        }

        // 2. Let matcher be ? Get(argument, @@match).
        let matcher = self.as_object().get_with_cache(
            vm,
            &PropertyKey::from(vm.well_known_symbols().match_),
            vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::ValueIsRegExp),
        )?;

        // 3. If matcher is not undefined, return ToBoolean(matcher).
        if !matcher.is_undefined() {
            return Ok(matcher.to_boolean());
        }

        // 4. If argument has a [[RegExpMatcher]] internal slot, return true.
        // 5. Return false.
        Ok(self.as_object().class().id == ClassId::RegExpObject)
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
        let realm = vm
            .current_realm()
            .expect("ToObject runs in an execution context with a realm");
        assert!(!self.is_empty());

        // Number
        if self.is_number() {
            // Return a new Number object whose [[NumberData]] internal slot is set to argument. See 21.1 for a description of Number objects.
            return Ok(NumberObject::create(vm, realm, self.as_f64()).upcast());
        }

        match self.tag() {
            // Undefined
            // Null
            nan_box::UNDEFINED_TAG | nan_box::NULL_TAG => {
                // Throw a TypeError exception.
                vm.throw_completion(ErrorKind::TypeError, ErrorType::ToObjectNullOrUndefined, &[])
            }
            // Boolean
            nan_box::BOOLEAN_TAG => {
                // Return a new Boolean object whose [[BooleanData]] internal slot is set to argument. See 20.3 for a description of Boolean objects.
                Ok(BooleanObject::create(vm, realm, self.as_bool()).upcast())
            }
            // String
            nan_box::STRING_TAG => {
                // Return a new String object whose [[StringData]] internal slot is set to argument. See 22.1 for a description of String objects.
                Ok(StringObject::create(vm, realm, self.as_string(), realm.intrinsics().string_prototype(vm)).upcast())
            }
            // Symbol
            nan_box::SYMBOL_TAG => {
                // Return a new Symbol object whose [[SymbolData]] internal slot is set to argument. See 20.4 for a description of Symbol objects.
                Ok(SymbolObject::create(vm, realm, self.as_symbol()).upcast())
            }
            // BigInt
            nan_box::BIGINT_TAG => {
                // Return a new BigInt object whose [[BigIntData]] internal slot is set to argument. See 21.2 for a description of BigInt objects.
                Ok(BigIntObject::create(vm, realm, self.as_bigint()).upcast())
            }
            // Object
            nan_box::OBJECT_TAG => {
                // Return argument.
                Ok(self.as_object())
            }
            _ => unreachable!("ToObject of a value that is not a language value"),
        }
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

    // 7.1.3 ToNumeric ( value ), https://tc39.es/ecma262/#sec-tonumeric
    #[inline]
    pub fn to_numeric(self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // OPTIMIZATION: Fast path for when this value is already a number.
        if self.is_number() {
            return Ok(self);
        }
        self.to_numeric_slow_case(vm)
    }

    #[cold]
    fn to_numeric_slow_case(self, vm: &Vm) -> ThrowCompletionOr<Value> {
        // OPTIMIZATION: Fast paths for some trivial common cases.
        if self.is_boolean() {
            return Ok(Value::from_i32(i32::from(self.as_bool())));
        }
        if self.is_null() {
            return Ok(Value::from_i32(0));
        }
        if self.is_undefined() {
            return Ok(Value::from_f64(f64::NAN));
        }

        // 1. Let primValue be ? ToPrimitive(value, number).
        let primitive_value = self.to_primitive(vm, PreferredType::Number)?;

        // 2. If primValue is a BigInt, return primValue.
        if primitive_value.is_bigint() {
            return Ok(primitive_value);
        }

        // 3. Return ? ToNumber(primValue).
        primitive_value.to_number(vm)
    }

    // 7.1.13 ToBigInt ( argument ), https://tc39.es/ecma262/#sec-tobigint
    pub fn to_bigint(self, vm: &Vm) -> ThrowCompletionOr<Gc<BigInt>> {
        // 1. Let prim be ? ToPrimitive(argument, number).
        let primitive = self.to_primitive(vm, PreferredType::Number)?;

        // 2. Return the value that prim corresponds to in Table 12.

        // Number
        if primitive.is_number() {
            // Throw a TypeError exception.
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::Convert, &[&"number", &"BigInt"]);
        }

        match primitive.tag() {
            // Undefined
            nan_box::UNDEFINED_TAG => {
                // Throw a TypeError exception.
                vm.throw_completion(ErrorKind::TypeError, ErrorType::Convert, &[&"undefined", &"BigInt"])
            }
            // Null
            nan_box::NULL_TAG => {
                // Throw a TypeError exception.
                vm.throw_completion(ErrorKind::TypeError, ErrorType::Convert, &[&"null", &"BigInt"])
            }
            // Boolean
            nan_box::BOOLEAN_TAG => {
                // Return 1n if prim is true and 0n if prim is false.
                let value = i32::from(primitive.as_bool());
                Ok(BigInt::create(vm, SignedBigInteger::from(value)))
            }
            // BigInt
            nan_box::BIGINT_TAG => {
                // Return prim.
                Ok(primitive.as_bigint())
            }
            nan_box::STRING_TAG => {
                // 1. Let n be ! StringToBigInt(prim).
                let bigint = string_to_bigint(vm, &primitive.as_string());

                // 2. If n is undefined, throw a SyntaxError exception.
                let Some(bigint) = bigint else {
                    return vm.throw_completion(ErrorKind::SyntaxError, ErrorType::BigIntInvalidValue, &[&primitive]);
                };

                // 3. Return n.
                Ok(bigint)
            }
            // Symbol
            nan_box::SYMBOL_TAG => {
                // Throw a TypeError exception.
                vm.throw_completion(ErrorKind::TypeError, ErrorType::Convert, &[&"symbol", &"BigInt"])
            }
            _ => unreachable!("ToBigInt of a value that is not a primitive language value"),
        }
    }

    // 7.1.15 ToBigInt64 ( argument ), https://tc39.es/ecma262/#sec-tobigint64
    pub fn to_bigint_int64(self, vm: &Vm) -> ThrowCompletionOr<i64> {
        // 1. Let n be ? ToBigInt(argument).
        let bigint = self.to_bigint(vm)?;

        // 2. Let int64bit be ℝ(n) modulo 2^64.
        // 3. If int64bit ≥ 2^63, return ℤ(int64bit - 2^64); otherwise return ℤ(int64bit).
        Ok(big_int_algorithms::to_u64(bigint.big_integer()) as i64)
    }

    // 7.1.16 ToBigUint64 ( argument ), https://tc39.es/ecma262/#sec-tobiguint64
    pub fn to_bigint_uint64(self, vm: &Vm) -> ThrowCompletionOr<u64> {
        // 1. Let n be ? ToBigInt(argument).
        let bigint = self.to_bigint(vm)?;

        // 2. Let int64bit be ℝ(n) modulo 2^64.
        // 3. Return ℤ(int64bit).
        Ok(big_int_algorithms::to_u64(bigint.big_integer()))
    }

    pub fn to_double(self, vm: &Vm) -> ThrowCompletionOr<f64> {
        Ok(self.to_number(vm)?.as_f64())
    }

    // 7.1.19 ToPropertyKey ( argument ), https://tc39.es/ecma262/#sec-topropertykey
    pub fn to_property_key(self, vm: &Vm) -> ThrowCompletionOr<PropertyKey> {
        // OPTIMIZATION: Return the value as a numeric PropertyKey, if possible.
        if self.is_non_negative_int32() {
            return Ok(PropertyKey::from_number(self.as_i32() as u64));
        }

        // OPTIMIZATION: If this is already a string, we can skip all the ceremony.
        if self.is_string() {
            return Ok(self.as_string().property_key(vm));
        }

        // 1. Let key be ? ToPrimitive(argument, string).
        let key = self.to_primitive(vm, PreferredType::String)?;

        // 2. If key is a Symbol, then
        if key.is_symbol() {
            // a. Return key.
            return Ok(PropertyKey::from_symbol(key.as_symbol()));
        }

        // OPTIMIZATION: Keep the atomized storage when ToPrimitive produced a string.
        if key.is_string() {
            return Ok(key.as_string().property_key(vm));
        }

        // 3. Return ! ToString(key).
        Ok(PropertyKey::from_utf16_string(&key.to_utf16_string(vm).must()))
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

    // 7.1.8 ToInt16 ( argument ), https://tc39.es/ecma262/#sec-toint16
    pub fn to_i16(self, vm: &Vm) -> ThrowCompletionOr<i16> {
        // 1. Let number be ? ToNumber(argument).
        let number = self.to_number(vm)?.as_f64();

        // 2-5.
        Ok(value_conversions::to_i16(number))
    }

    // 7.1.9 ToUint16 ( argument ), https://tc39.es/ecma262/#sec-touint16
    pub fn to_u16(self, vm: &Vm) -> ThrowCompletionOr<u16> {
        // 1. Let number be ? ToNumber(argument).
        let number = self.to_number(vm)?.as_f64();

        // 2-5.
        Ok(value_conversions::to_u16(number))
    }

    // 7.1.10 ToInt8 ( argument ), https://tc39.es/ecma262/#sec-toint8
    pub fn to_i8(self, vm: &Vm) -> ThrowCompletionOr<i8> {
        // 1. Let number be ? ToNumber(argument).
        let number = self.to_number(vm)?.as_f64();

        // 2-5.
        Ok(value_conversions::to_i8(number))
    }

    // 7.1.11 ToUint8 ( argument ), https://tc39.es/ecma262/#sec-touint8
    pub fn to_u8(self, vm: &Vm) -> ThrowCompletionOr<u8> {
        // OPTIMIZATION: Fast path for the common case of an int32.
        if self.is_int32() {
            return Ok((self.as_i32() & i32::from(u8::MAX)) as u8);
        }

        // 1. Let number be ? ToNumber(argument).
        let number = self.to_number(vm)?.as_f64();

        // 2-5.
        Ok(value_conversions::to_u8(number))
    }

    // 7.1.12 ToUint8Clamp ( argument ), https://tc39.es/ecma262/#sec-touint8clamp
    pub fn to_u8_clamp(self, vm: &Vm) -> ThrowCompletionOr<u8> {
        // 1. Let number be ? ToNumber(argument).
        let number = self.to_number(vm)?.as_f64();

        // 2-9.
        Ok(value_conversions::to_u8_clamp(number))
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

    // 7.1.22 ToIndex ( argument ), https://tc39.es/ecma262/#sec-toindex
    pub fn to_index(self, vm: &Vm) -> ThrowCompletionOr<u64> {
        // 1. If value is undefined, then
        if self.is_undefined() {
            // a. Return 0.
            return Ok(0);
        }

        // 2. Else,
        // a. Let integer be ? ToIntegerOrInfinity(value).
        let number = self.to_number(vm)?.as_f64();

        // b-e.
        value_conversions::to_index(number).or_else(|error| error.throw_completion(vm))
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
            return number_to_ascii_utf16_string(self.as_f64());
        }

        match self.tag() {
            nan_box::UNDEFINED_TAG => Utf16String::from_utf8("undefined"),
            nan_box::NULL_TAG => Utf16String::from_utf8("null"),
            nan_box::BOOLEAN_TAG => Utf16String::from_utf8(if self.as_bool() { "true" } else { "false" }),
            nan_box::INT32_TAG => integer_to_utf16_string(self.as_i32().into()),
            nan_box::STRING_TAG => self.as_string().utf16_string(),
            nan_box::SYMBOL_TAG => self.as_symbol().descriptive_string(),
            nan_box::BIGINT_TAG => self.as_bigint().to_utf16_string(),
            nan_box::OBJECT_TAG => {
                Utf16String::from_utf8(&format!("[object {}]", self.as_object().class().class_name()))
            }
            nan_box::ACCESSOR_TAG => Utf16String::from_utf8("<accessor>"),
            nan_box::EMPTY_TAG => Utf16String::from_utf8("<empty>"),
            _ => unreachable!("a value with an unknown tag"),
        }
    }

    pub fn to_primitive_string(self, vm: &Vm) -> ThrowCompletionOr<Gc<PrimitiveString>> {
        if self.is_string() {
            return Ok(self.as_string());
        }
        if self.is_non_negative_int32() {
            return Ok(PrimitiveString::create_from_unsigned_integer(
                vm,
                u64::from(self.as_i32() as u32),
            ));
        }
        let string = self.to_utf16_string(vm)?;
        Ok(PrimitiveString::create(vm, string))
    }

    // 7.1.17 ToString ( argument ), https://tc39.es/ecma262/#sec-tostring
    pub fn to_utf16_string(self, vm: &Vm) -> ThrowCompletionOr<Utf16String> {
        if self.is_double() {
            return Ok(number_to_ascii_utf16_string(self.as_f64()));
        }

        match self.tag() {
            // 1. If argument is a String, return argument.
            nan_box::STRING_TAG => Ok(self.as_string().utf16_string()),
            // 2. If argument is a Symbol, throw a TypeError exception.
            nan_box::SYMBOL_TAG => {
                vm.throw_completion(ErrorKind::TypeError, ErrorType::Convert, &[&"symbol", &"string"])
            }
            // 3. If argument is undefined, return "undefined".
            nan_box::UNDEFINED_TAG => Ok(Utf16String::from_utf8("undefined")),
            // 4. If argument is null, return "null".
            nan_box::NULL_TAG => Ok(Utf16String::from_utf8("null")),
            // 5. If argument is true, return "true".
            // 6. If argument is false, return "false".
            nan_box::BOOLEAN_TAG => Ok(Utf16String::from_utf8(if self.as_bool() { "true" } else { "false" })),
            // 7. If argument is a Number, return Number::toString(argument, 10).
            nan_box::INT32_TAG => Ok(integer_to_utf16_string(self.as_i32().into())),
            // 8. If argument is a BigInt, return BigInt::toString(argument, 10).
            nan_box::BIGINT_TAG => Ok(Utf16String::from_utf8(&big_int_algorithms::to_base(
                self.as_bigint().big_integer(),
                10,
            ))),
            // 9. Assert: argument is an Object.
            nan_box::OBJECT_TAG => {
                // 10. Let primValue be ? ToPrimitive(argument, string).
                let primitive_value = self.to_primitive(vm, PreferredType::String)?;

                // 11. Assert: primValue is not an Object.
                assert!(!primitive_value.is_object());

                // 12. Return ? ToString(primValue).
                primitive_value.to_utf16_string(vm)
            }
            _ => unreachable!("ToString of a value that is not a language value"),
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
                current_realm().string_prototype(vm)
            } else if self.is_boolean() {
                current_realm().boolean_prototype(vm)
            } else if self.is_number() {
                current_realm().number_prototype(vm)
            } else if self.is_bigint() {
                current_realm().bigint_prototype(vm)
            } else if self.is_symbol() {
                current_realm().symbol_prototype(vm)
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

    // 7.3.21 Invoke ( V, P [ , argumentsList ] ), https://tc39.es/ecma262/#sec-invoke
    pub fn invoke(self, vm: &Vm, property_key: &PropertyKey, arguments: &[Value]) -> ThrowCompletionOr<Value> {
        // 1. If argumentsList is not present, set argumentsList to a new empty List.

        // 2. Let func be ? GetV(V, P).
        let function = self.get(vm, property_key)?;

        // 3. Return ? Call(func, V, argumentsList).
        call(vm, function, self, arguments)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PreferredType {
    Default,
    String,
    Number,
}

/// Mirrors AK::TriState, which IsLessThan returns for its undefined result.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TriState {
    False,
    True,
    Unknown,
}

fn both_number(lhs: Value, rhs: Value) -> bool {
    lhs.is_number() && rhs.is_number()
}

fn both_bigint(lhs: Value, rhs: Value) -> bool {
    lhs.is_bigint() && rhs.is_bigint()
}

// 7.1.14 StringToBigInt ( str ), https://tc39.es/ecma262/#sec-stringtobigint
fn string_to_bigint(vm: &Vm, string: &PrimitiveString) -> Option<Gc<BigInt>> {
    let utf16_string = string.utf16_string();
    let code_units = utf16_string.to_utf16();

    // 1-5.
    let bigint = value_conversions::string_to_bigint(&code_units)?;

    // 6. Return ℤ(mv).
    Some(BigInt::create(vm, bigint))
}

fn create_bigint_value(vm: &Vm, big_integer: SignedBigInteger) -> Value {
    Value::from_bigint(BigInt::create(vm, big_integer))
}

// 13.10 Relational Operators, https://tc39.es/ecma262/#sec-relational-operators
// RelationalExpression : RelationalExpression > ShiftExpression
pub fn greater_than(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<bool> {
    // 1. Let lref be ? Evaluation of RelationalExpression.
    // 2. Let lval be ? GetValue(lref).
    // 3. Let rref be ? Evaluation of ShiftExpression.
    // 4. Let rval be ? GetValue(rref).
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // 5. Let r be ? IsLessThan(rval, lval, false).
    let relation = is_less_than(vm, lhs, rhs, false)?;

    // 6. If r is undefined, return false. Otherwise, return r.
    if relation == TriState::Unknown {
        return Ok(false);
    }
    Ok(relation == TriState::True)
}

// 13.10 Relational Operators, https://tc39.es/ecma262/#sec-relational-operators
// RelationalExpression : RelationalExpression >= ShiftExpression
pub fn greater_than_equals(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<bool> {
    // 1. Let lref be ? Evaluation of RelationalExpression.
    // 2. Let lval be ? GetValue(lref).
    // 3. Let rref be ? Evaluation of ShiftExpression.
    // 4. Let rval be ? GetValue(rref).
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // 5. Let r be ? IsLessThan(lval, rval, true).
    let relation = is_less_than(vm, lhs, rhs, true)?;

    // 6. If r is true or undefined, return false. Otherwise, return true.
    if relation == TriState::Unknown || relation == TriState::True {
        return Ok(false);
    }
    Ok(true)
}

// 13.10 Relational Operators, https://tc39.es/ecma262/#sec-relational-operators
// RelationalExpression : RelationalExpression < ShiftExpression
pub fn less_than(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<bool> {
    // 1. Let lref be ? Evaluation of RelationalExpression.
    // 2. Let lval be ? GetValue(lref).
    // 3. Let rref be ? Evaluation of ShiftExpression.
    // 4. Let rval be ? GetValue(rref).
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // 5. Let r be ? IsLessThan(lval, rval, true).
    let relation = is_less_than(vm, lhs, rhs, true)?;

    // 6. If r is undefined, return false. Otherwise, return r.
    if relation == TriState::Unknown {
        return Ok(false);
    }
    Ok(relation == TriState::True)
}

// 13.10 Relational Operators, https://tc39.es/ecma262/#sec-relational-operators
// RelationalExpression : RelationalExpression <= ShiftExpression
pub fn less_than_equals(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<bool> {
    // 1. Let lref be ? Evaluation of RelationalExpression.
    // 2. Let lval be ? GetValue(lref).
    // 3. Let rref be ? Evaluation of ShiftExpression.
    // 4. Let rval be ? GetValue(rref).
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // 5. Let r be ? IsLessThan(rval, lval, false).
    let relation = is_less_than(vm, lhs, rhs, false)?;

    // 6. If r is true or undefined, return false. Otherwise, return true.
    if relation == TriState::True || relation == TriState::Unknown {
        return Ok(false);
    }
    Ok(true)
}

// 13.12 Binary Bitwise Operators, https://tc39.es/ecma262/#sec-binary-bitwise-operators
// BitwiseANDExpression : BitwiseANDExpression & EqualityExpression
pub fn bitwise_and(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    let lhs_numeric = lhs.to_numeric(vm)?;

    // 4. Let rnum be ? ToNumeric(rval).
    let rhs_numeric = rhs.to_numeric(vm)?;

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if both_number(lhs_numeric, rhs_numeric) {
        // 6.1.6.1.17 Number::bitwiseAND ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-bitwiseAND
        // 1. Return NumberBitwiseOp(&, x, y).
        if !lhs_numeric.is_finite_number() || !rhs_numeric.is_finite_number() {
            return Ok(Value::from_i32(0));
        }
        return Ok(Value::from_i32(lhs_numeric.to_i32(vm)? & rhs_numeric.to_i32(vm)?));
    }
    if both_bigint(lhs_numeric, rhs_numeric) {
        // 6.1.6.2.18 BigInt::bitwiseAND ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-bitwiseAND
        // 1. Return BigIntBitwiseOp(&, x, y).
        let result = lhs_numeric.as_bigint().big_integer() & rhs_numeric.as_bigint().big_integer();
        return Ok(create_bigint_value(vm, result));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::BigIntBadOperatorOtherType,
        &[&"bitwise AND"],
    )
}

// 13.12 Binary Bitwise Operators, https://tc39.es/ecma262/#sec-binary-bitwise-operators
// BitwiseORExpression : BitwiseORExpression | BitwiseXORExpression
pub fn bitwise_or(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    let lhs_numeric = lhs.to_numeric(vm)?;

    // 4. Let rnum be ? ToNumeric(rval).
    let rhs_numeric = rhs.to_numeric(vm)?;

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if both_number(lhs_numeric, rhs_numeric) {
        // 6.1.6.1.19 Number::bitwiseOR ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-bitwiseOR
        // 1. Return NumberBitwiseOp(|, x, y).
        return Ok(Value::from_i32(lhs_numeric.to_i32(vm)? | rhs_numeric.to_i32(vm)?));
    }
    if both_bigint(lhs_numeric, rhs_numeric) {
        // 6.1.6.2.20 BigInt::bitwiseOR ( x, y )
        // 1. Return BigIntBitwiseOp(|, x, y).
        let result = lhs_numeric.as_bigint().big_integer() | rhs_numeric.as_bigint().big_integer();
        return Ok(create_bigint_value(vm, result));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::BigIntBadOperatorOtherType,
        &[&"bitwise OR"],
    )
}

// 13.12 Binary Bitwise Operators, https://tc39.es/ecma262/#sec-binary-bitwise-operators
// BitwiseXORExpression : BitwiseXORExpression ^ BitwiseANDExpression
pub fn bitwise_xor(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    let lhs_numeric = lhs.to_numeric(vm)?;

    // 4. Let rnum be ? ToNumeric(rval).
    let rhs_numeric = rhs.to_numeric(vm)?;

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if both_number(lhs_numeric, rhs_numeric) {
        // 6.1.6.1.18 Number::bitwiseXOR ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-bitwiseXOR
        // 1. Return NumberBitwiseOp(^, x, y).
        return Ok(Value::from_i32(lhs_numeric.to_i32(vm)? ^ rhs_numeric.to_i32(vm)?));
    }
    if both_bigint(lhs_numeric, rhs_numeric) {
        // 6.1.6.2.19 BigInt::bitwiseXOR ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-bitwiseXOR
        // 1. Return BigIntBitwiseOp(^, x, y).
        let result = lhs_numeric.as_bigint().big_integer() ^ rhs_numeric.as_bigint().big_integer();
        return Ok(create_bigint_value(vm, result));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::BigIntBadOperatorOtherType,
        &[&"bitwise XOR"],
    )
}

// 13.5.6 Bitwise NOT Operator ( ~ ), https://tc39.es/ecma262/#sec-bitwise-not-operator
// UnaryExpression : ~ UnaryExpression
pub fn bitwise_not(vm: &Vm, lhs: Value) -> ThrowCompletionOr<Value> {
    // 1. Let expr be ? Evaluation of UnaryExpression.
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // 2. Let oldValue be ? ToNumeric(? GetValue(expr)).
    let old_value = lhs.to_numeric(vm)?;

    // 3. If oldValue is a Number, then
    if old_value.is_number() {
        // a. Return Number::bitwiseNOT(oldValue).

        // 6.1.6.1.2 Number::bitwiseNOT ( x ), https://tc39.es/ecma262/#sec-numeric-types-number-bitwiseNOT
        // 1. Let oldValue be ! ToInt32(x).
        // 2. Return the result of applying bitwise complement to oldValue. The mathematical value of the result is
        //    exactly representable as a 32-bit two's complement bit string.
        return Ok(Value::from_i32(!old_value.to_i32(vm)?));
    }

    // 4. Else,
    // a. Assert: oldValue is a BigInt.
    assert!(old_value.is_bigint());

    // b. Return BigInt::bitwiseNOT(oldValue).

    // 6.1.6.2.2 BigInt::bitwiseNOT ( x ), https://tc39.es/ecma262/#sec-numeric-types-bigint-bitwiseNOT
    // 1. Return -x - 1ℤ.
    let result = !old_value.as_bigint().big_integer();
    Ok(create_bigint_value(vm, result))
}

// 13.5.4 Unary + Operator, https://tc39.es/ecma262/#sec-unary-plus-operator
// UnaryExpression : + UnaryExpression
pub fn unary_plus(vm: &Vm, lhs: Value) -> ThrowCompletionOr<Value> {
    // 1. Let expr be ? Evaluation of UnaryExpression.
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // 2. Return ? ToNumber(? GetValue(expr)).
    lhs.to_number(vm)
}

// 13.5.5 Unary - Operator, https://tc39.es/ecma262/#sec-unary-minus-operator
// UnaryExpression : - UnaryExpression
pub fn unary_minus(vm: &Vm, lhs: Value) -> ThrowCompletionOr<Value> {
    // 1. Let expr be ? Evaluation of UnaryExpression.
    // NOTE: This is handled in the AST or Bytecode interpreter.

    // 2. Let oldValue be ? ToNumeric(? GetValue(expr)).
    let old_value = lhs.to_numeric(vm)?;

    // 3. If oldValue is a Number, then
    if old_value.is_number() {
        // a. Return Number::unaryMinus(oldValue).

        // 6.1.6.1.1 Number::unaryMinus ( x ), https://tc39.es/ecma262/#sec-numeric-types-number-unaryMinus
        // 1. If x is NaN, return NaN.
        if old_value.is_nan() {
            return Ok(Value::from_f64(f64::NAN));
        }

        // 2. Return the result of negating x; that is, compute a Number with the same magnitude but opposite sign.
        return Ok(Value::from_f64(-old_value.as_f64()));
    }

    // 4. Else,
    // a. Assert: oldValue is a BigInt.
    assert!(old_value.is_bigint());

    // b. Return BigInt::unaryMinus(oldValue).

    // 6.1.6.2.1 BigInt::unaryMinus ( x ), https://tc39.es/ecma262/#sec-numeric-types-bigint-unaryMinus
    // 1. If x is 0ℤ, return 0ℤ.
    if old_value.as_bigint().big_integer().is_zero() {
        return Ok(create_bigint_value(vm, SignedBigInteger::zero()));
    }

    // 2. Return the BigInt value that represents the negation of ℝ(x).
    let big_integer_negated = -old_value.as_bigint().big_integer();
    Ok(create_bigint_value(vm, big_integer_negated))
}

// 13.9.1 The Left Shift Operator ( << ), https://tc39.es/ecma262/#sec-left-shift-operator
// ShiftExpression : ShiftExpression << AdditiveExpression
pub fn left_shift(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    let lhs_numeric = lhs.to_numeric(vm)?;

    // 4. Let rnum be ? ToNumeric(rval).
    let rhs_numeric = rhs.to_numeric(vm)?;

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if both_number(lhs_numeric, rhs_numeric) {
        // 6.1.6.1.9 Number::leftShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-leftShift

        // 1. Let lnum be ! ToInt32(x).
        let lhs_i32 = lhs_numeric.to_i32(vm).must();

        // 2. Let rnum be ! ToUint32(y).
        let rhs_u32 = rhs_numeric.to_u32(vm).must();

        // 3. Let shiftCount be ℝ(rnum) modulo 32.
        let shift_count = rhs_u32 % 32;

        // 4. Return the result of left shifting lnum by shiftCount bits. The mathematical value of the result is
        //    exactly representable as a 32-bit two's complement bit string.
        return Ok(Value::from_i32(lhs_i32 << shift_count));
    }
    if both_bigint(lhs_numeric, rhs_numeric) {
        // AD-HOC: Prevent allocating huge amounts of memory.
        // 6.1.6.2.9 BigInt::leftShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-leftShift
        // 1. If y < 0ℤ, then
        //    a. Return the BigInt value that represents ℝ(x) / 2^-y, rounding down to the nearest integer, including for negative numbers.
        // 2. Return the BigInt value that represents ℝ(x) × 2^y.
        let result = big_int_algorithms::left_shift(
            lhs_numeric.as_bigint().big_integer(),
            rhs_numeric.as_bigint().big_integer(),
        );
        return match result {
            Ok(result) => Ok(create_bigint_value(vm, result)),
            Err(error) => error.throw_completion(vm),
        };
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::BigIntBadOperatorOtherType,
        &[&"left-shift"],
    )
}

// 13.9.2 The Signed Right Shift Operator ( >> ), https://tc39.es/ecma262/#sec-signed-right-shift-operator
// ShiftExpression : ShiftExpression >> AdditiveExpression
pub fn right_shift(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    let lhs_numeric = lhs.to_numeric(vm)?;

    // 4. Let rnum be ? ToNumeric(rval).
    let rhs_numeric = rhs.to_numeric(vm)?;

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if both_number(lhs_numeric, rhs_numeric) {
        // 6.1.6.1.10 Number::signedRightShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-signedRightShift

        // 1. Let lnum be ! ToInt32(x).
        let lhs_i32 = lhs_numeric.to_i32(vm).must();

        // 2. Let rnum be ! ToUint32(y).
        let rhs_u32 = rhs_numeric.to_u32(vm).must();

        // 3. Let shiftCount be ℝ(rnum) modulo 32.
        let shift_count = rhs_u32 % 32;

        // 4. Return the result of performing a sign-extending right shift of lnum by shiftCount bits.
        //    The most significant bit is propagated. The mathematical value of the result is exactly representable
        //    as a 32-bit two's complement bit string.
        return Ok(Value::from_i32(lhs_i32 >> shift_count));
    }
    if both_bigint(lhs_numeric, rhs_numeric) {
        // 6.1.6.2.10 BigInt::signedRightShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-signedRightShift
        // 1. Return BigInt::leftShift(x, -y).
        let rhs_negated = -rhs_numeric.as_bigint().big_integer();
        // NOTE: This passes the original left operand, which converts it with ToNumeric again.
        return left_shift(vm, lhs, create_bigint_value(vm, rhs_negated));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::BigIntBadOperatorOtherType,
        &[&"right-shift"],
    )
}

// 13.9.3 The Unsigned Right Shift Operator ( >>> ), https://tc39.es/ecma262/#sec-unsigned-right-shift-operator
// ShiftExpression : ShiftExpression >>> AdditiveExpression
pub fn unsigned_right_shift(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 5-6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    let lhs_numeric = lhs.to_numeric(vm)?;

    // 4. Let rnum be ? ToNumeric(rval).
    let rhs_numeric = rhs.to_numeric(vm)?;

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if both_number(lhs_numeric, rhs_numeric) {
        // 6.1.6.1.11 Number::unsignedRightShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-unsignedRightShift

        // 1. Let lnum be ! ToUint32(x).
        let lhs_u32 = lhs_numeric.to_u32(vm).must();

        // 2. Let rnum be ! ToUint32(y).
        let rhs_u32 = rhs_numeric.to_u32(vm).must();

        // 3. Let shiftCount be ℝ(rnum) modulo 32.
        let shift_count = rhs_u32 % 32;

        // 4. Return the result of performing a zero-filling right shift of lnum by shiftCount bits.
        //    Vacated bits are filled with zero. The mathematical value of the result is exactly representable
        //    as a 32-bit unsigned bit string.
        return Ok(Value::from_f64(f64::from(lhs_u32 >> shift_count)));
    }

    // 6. If lnum is a BigInt, then
    // d. If opText is >>>, return ? BigInt::unsignedRightShift(lnum, rnum).

    // 6.1.6.2.11 BigInt::unsignedRightShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-unsignedRightShift
    // 1. Throw a TypeError exception.
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::BigIntBadOperator,
        &[&"unsigned right-shift"],
    )
}

// 13.8.1 The Addition Operator ( + ), https://tc39.es/ecma262/#sec-addition-operator-plus
// AdditiveExpression : AdditiveExpression + MultiplicativeExpression
pub fn add(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator

    // 1. If opText is +, then

    // a. Let lprim be ? ToPrimitive(lval).
    let lhs_primitive = lhs.to_primitive(vm, PreferredType::Default)?;

    // b. Let rprim be ? ToPrimitive(rval).
    let rhs_primitive = rhs.to_primitive(vm, PreferredType::Default)?;

    // c. If lprim is a String or rprim is a String, then
    if lhs_primitive.is_string() || rhs_primitive.is_string() {
        // i. Let lstr be ? ToString(lprim).
        let lhs_string = lhs_primitive.to_primitive_string(vm)?;

        // ii. Let rstr be ? ToString(rprim).
        let rhs_string = rhs_primitive.to_primitive_string(vm)?;

        // iii. Return the string-concatenation of lstr and rstr.
        return Ok(Value::from_string(PrimitiveString::create_from_concatenation(
            vm, lhs_string, rhs_string,
        )?));
    }

    // d. Set lval to lprim.
    // e. Set rval to rprim.

    // 2. NOTE: At this point, it must be a numeric operation.

    // 3. Let lnum be ? ToNumeric(lval).
    let lhs_numeric = lhs_primitive.to_numeric(vm)?;

    // 4. Let rnum be ? ToNumeric(rval).
    let rhs_numeric = rhs_primitive.to_numeric(vm)?;

    // 6. N/A.

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if both_number(lhs_numeric, rhs_numeric) {
        // 6.1.6.1.7 Number::add ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-add
        let x = lhs_numeric.as_f64();
        let y = rhs_numeric.as_f64();
        return Ok(Value::from_f64(x + y));
    }
    if both_bigint(lhs_numeric, rhs_numeric) {
        // 6.1.6.2.7 BigInt::add ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-add
        let result = lhs_numeric.as_bigint().big_integer() + rhs_numeric.as_bigint().big_integer();
        return Ok(create_bigint_value(vm, result));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::BigIntBadOperatorOtherType,
        &[&"addition"],
    )
}

// 13.8.2 The Subtraction Operator ( - ), https://tc39.es/ecma262/#sec-subtraction-operator-minus
// AdditiveExpression : AdditiveExpression - MultiplicativeExpression
pub fn sub(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    let lhs_numeric = lhs.to_numeric(vm)?;

    // 4. Let rnum be ? ToNumeric(rval).
    let rhs_numeric = rhs.to_numeric(vm)?;

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if both_number(lhs_numeric, rhs_numeric) {
        // 6.1.6.1.8 Number::subtract ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-subtract
        let x = lhs_numeric.as_f64();
        let y = rhs_numeric.as_f64();
        // 1. Return Number::add(x, Number::unaryMinus(y)).
        return Ok(Value::from_f64(x - y));
    }
    if both_bigint(lhs_numeric, rhs_numeric) {
        // 6.1.6.2.8 BigInt::subtract ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-subtract
        // 1. Return the BigInt value that represents the difference x minus y.
        let result = lhs_numeric.as_bigint().big_integer() - rhs_numeric.as_bigint().big_integer();
        return Ok(create_bigint_value(vm, result));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::BigIntBadOperatorOtherType,
        &[&"subtraction"],
    )
}

// 13.7 Multiplicative Operators, https://tc39.es/ecma262/#sec-multiplicative-operators
// MultiplicativeExpression : MultiplicativeExpression MultiplicativeOperator ExponentiationExpression
pub fn mul(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    let lhs_numeric = lhs.to_numeric(vm)?;

    // 4. Let rnum be ? ToNumeric(rval).
    let rhs_numeric = rhs.to_numeric(vm)?;

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if both_number(lhs_numeric, rhs_numeric) {
        // 6.1.6.1.4 Number::multiply ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-multiply
        let x = lhs_numeric.as_f64();
        let y = rhs_numeric.as_f64();
        return Ok(Value::from_f64(x * y));
    }
    if both_bigint(lhs_numeric, rhs_numeric) {
        // 6.1.6.2.4 BigInt::multiply ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-multiply
        // 1. Return the BigInt value that represents the product of x and y.
        let result = lhs_numeric.as_bigint().big_integer() * rhs_numeric.as_bigint().big_integer();
        return Ok(create_bigint_value(vm, result));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::BigIntBadOperatorOtherType,
        &[&"multiplication"],
    )
}

// 13.7 Multiplicative Operators, https://tc39.es/ecma262/#sec-multiplicative-operators
// MultiplicativeExpression : MultiplicativeExpression MultiplicativeOperator ExponentiationExpression
pub fn div(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    let lhs_numeric = lhs.to_numeric(vm)?;

    // 4. Let rnum be ? ToNumeric(rval).
    let rhs_numeric = rhs.to_numeric(vm)?;

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if both_number(lhs_numeric, rhs_numeric) {
        // 6.1.6.1.5 Number::divide ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-divide
        return Ok(Value::from_f64(lhs_numeric.as_f64() / rhs_numeric.as_f64()));
    }
    if both_bigint(lhs_numeric, rhs_numeric) {
        // 6.1.6.2.5 BigInt::divide ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-divide
        // 1. If y is 0ℤ, throw a RangeError exception.
        if rhs_numeric.as_bigint().big_integer().is_zero() {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::DivisionByZero, &[]);
        }
        // 2. Let quotient be ℝ(x) / ℝ(y).
        // 3. Return the BigInt value that represents quotient rounded towards 0 to the next integer value.
        let result = lhs_numeric.as_bigint().big_integer() / rhs_numeric.as_bigint().big_integer();
        return Ok(create_bigint_value(vm, result));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::BigIntBadOperatorOtherType,
        &[&"division"],
    )
}

// 13.7 Multiplicative Operators, https://tc39.es/ecma262/#sec-multiplicative-operators
// MultiplicativeExpression : MultiplicativeExpression MultiplicativeOperator ExponentiationExpression
pub fn r#mod(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 13.15.3 ApplyStringOrNumericBinaryOperator ( lval, opText, rval ), https://tc39.es/ecma262/#sec-applystringornumericbinaryoperator
    // 1-2, 6. N/A.

    // 3. Let lnum be ? ToNumeric(lval).
    let lhs_numeric = lhs.to_numeric(vm)?;

    // 4. Let rnum be ? ToNumeric(rval).
    let rhs_numeric = rhs.to_numeric(vm)?;

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if both_number(lhs_numeric, rhs_numeric) {
        // 6.1.6.1.6 Number::remainder ( n, d ), https://tc39.es/ecma262/#sec-numeric-types-number-remainder
        // The ECMA specification is describing the mathematical definition of modulus
        // implemented by fmod.
        let n = lhs_numeric.as_f64();
        let d = rhs_numeric.as_f64();
        return Ok(Value::from_f64(n % d));
    }
    if both_bigint(lhs_numeric, rhs_numeric) {
        // 6.1.6.2.6 BigInt::remainder ( n, d ), https://tc39.es/ecma262/#sec-numeric-types-bigint-remainder
        // 1. If d is 0ℤ, throw a RangeError exception.
        if rhs_numeric.as_bigint().big_integer().is_zero() {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::DivisionByZero, &[]);
        }
        // 2. If n is 0ℤ, return 0ℤ.
        // 3. Let quotient be ℝ(n) / ℝ(d).
        // 4. Let q be the BigInt whose sign is the sign of quotient and whose magnitude is floor(abs(quotient)).
        // 5. Return n - (d × q).
        let result = lhs_numeric.as_bigint().big_integer() % rhs_numeric.as_bigint().big_integer();
        return Ok(create_bigint_value(vm, result));
    }

    // 5. If Type(lnum) is different from Type(rnum), throw a TypeError exception.
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::BigIntBadOperatorOtherType,
        &[&"modulo"],
    )
}

// 13.6 Exponentiation Operator, https://tc39.es/ecma262/#sec-exp-operator
// ExponentiationExpression : UpdateExpression ** ExponentiationExpression
pub fn exp(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 3. Let lnum be ? ToNumeric(lval).
    let lhs_numeric = lhs.to_numeric(vm)?;

    // 4. Let rnum be ? ToNumeric(rval).
    let rhs_numeric = rhs.to_numeric(vm)?;

    // 7. Let operation be the abstract operation associated with opText and Type(lnum) in the following table:
    // [...]
    // 8. Return operation(lnum, rnum).
    if both_number(lhs_numeric, rhs_numeric) {
        return Ok(Value::from_f64(value_conversions::exp_double(
            lhs_numeric.as_f64(),
            rhs_numeric.as_f64(),
        )));
    }
    if both_bigint(lhs_numeric, rhs_numeric) {
        // 6.1.6.2.3 BigInt::exponentiate ( base, exponent ), https://tc39.es/ecma262/#sec-numeric-types-bigint-exponentiate
        // 1. If exponent < 0ℤ, throw a RangeError exception.
        // AD-HOC: Prevent allocating huge amounts of memory.
        // 2. If base is 0ℤ and exponent is 0ℤ, return 1ℤ.
        // 3. Return the BigInt value that represents ℝ(base) raised to the power ℝ(exponent).
        let result = big_int_algorithms::exponentiate(
            lhs_numeric.as_bigint().big_integer(),
            rhs_numeric.as_bigint().big_integer(),
        );
        return match result {
            Ok(result) => Ok(create_bigint_value(vm, result)),
            Err(error) => error.throw_completion(vm),
        };
    }
    vm.throw_completion(
        ErrorKind::TypeError,
        ErrorType::BigIntBadOperatorOtherType,
        &[&"exponentiation"],
    )
}

pub fn r#in(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    if !rhs.is_object() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::InOperatorWithObject, &[]);
    }
    let lhs_property_key = lhs.to_property_key(vm)?;
    Ok(Value::from_bool(rhs.as_object().has_property(vm, &lhs_property_key)?))
}

/// Whether a function is the native function the realm installs as %Function.prototype%[@@hasInstance]. Only
/// NativeFunction gives a function a builtin, so the builtin alone identifies it.
fn is_ordinary_has_instance_builtin(function: Gc<FunctionObject>) -> bool {
    // SAFETY: A Gc points to a live cell, and every function object starts with its FunctionObject.
    let function = unsafe { function.as_non_null().as_ref() };
    function.has_builtin.get() && Builtin::from_u8(function.builtin.get()) == Some(Builtin::OrdinaryHasInstance)
}

/// BoundFunction::bound_target_function() of a function that is a bound function exotic object.
fn bound_target_function(function: Gc<FunctionObject>) -> Option<Gc<FunctionObject>> {
    function
        .downcast::<BoundFunction>()
        .map(|bound_function| bound_function.bound_target_function())
}

// 13.10.2 InstanceofOperator ( V, target ), https://tc39.es/ecma262/#sec-instanceofoperator
pub fn instance_of(vm: &Vm, value: Value, target: Value) -> ThrowCompletionOr<Value> {
    // 1. If target is not an Object, throw a TypeError exception.
    if !target.is_object() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&target]);
    }

    // 2. Let instOfHandler be ? GetMethod(target, @@hasInstance).
    let instance_of_handler = target.get_method_with_cache(
        vm,
        &PropertyKey::from(vm.well_known_symbols().has_instance),
        vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::InstanceOfHasInstance),
    )?;

    // 3. If instOfHandler is not undefined, then
    if let Some(instance_of_handler) = instance_of_handler {
        // OPTIMIZATION: If the handler is the default OrdinaryHasInstance, we can skip doing a generic call.
        if is_ordinary_has_instance_builtin(instance_of_handler) {
            return ordinary_has_instance(vm, value, target);
        }
        // a. Return ToBoolean(? Call(instOfHandler, target, « V »)).
        return Ok(Value::from_bool(
            call_function_object(vm, instance_of_handler, target, &[value])?.to_boolean(),
        ));
    }

    // 4. If IsCallable(target) is false, throw a TypeError exception.
    if !target.is_function() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&target]);
    }

    // 5. Return ? OrdinaryHasInstance(target, V).
    // NOTE: This passes target as the instance and V as the constructor.
    ordinary_has_instance(vm, target, value)
}

// 7.3.22 OrdinaryHasInstance ( C, O ), https://tc39.es/ecma262/#sec-ordinaryhasinstance
// NOTE: lhs is O and rhs is C.
pub fn ordinary_has_instance(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<Value> {
    // 1. If IsCallable(C) is false, return false.
    if !rhs.is_function() {
        return Ok(Value::FALSE);
    }

    let rhs_function = rhs.as_function();

    // 2. If C has a [[BoundTargetFunction]] internal slot, then
    if let Some(bound_target) = bound_target_function(rhs_function) {
        // a. Let BC be C.[[BoundTargetFunction]].
        // b. Return ? InstanceofOperator(O, BC).
        return instance_of(vm, lhs, Value::from_object(bound_target));
    }

    // 3. If O is not an Object, return false.
    if !lhs.is_object() {
        return Ok(Value::FALSE);
    }

    let mut lhs_object = lhs.as_object();

    // 4. Let P be ? Get(C, "prototype").
    let rhs_prototype = rhs.get_with_cache(
        vm,
        &vm.names.prototype,
        vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::OrdinaryHasInstancePrototype),
    )?;

    // 5. If P is not an Object, throw a TypeError exception.
    if !rhs_prototype.is_object() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::InstanceOfOperatorBadPrototype, &[&rhs]);
    }

    // 6. Repeat,
    loop {
        // a. Set O to ? O.[[GetPrototypeOf]]().
        // b. If O is null, return false.
        let Some(prototype) = lhs_object.internal_get_prototype_of(vm)? else {
            return Ok(Value::FALSE);
        };
        lhs_object = prototype;

        // c. If SameValue(P, O) is true, return true.
        if same_value(rhs_prototype, Value::from_object(lhs_object)) {
            return Ok(Value::TRUE);
        }
    }
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

// 7.2.15 IsStrictlyEqual ( x, y ), https://tc39.es/ecma262/#sec-isstrictlyequal
pub fn is_strictly_equal(lhs: Value, rhs: Value) -> bool {
    // 1. If Type(x) is different from Type(y), return false.
    if !same_type_for_equality(lhs, rhs) {
        return false;
    }

    // 2. If x is a Number, then
    if lhs.is_number() {
        // a. Return Number::equal(x, y).

        // 6.1.6.1.13 Number::equal ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-number-equal
        // 1. If x is NaN, return false.
        // 2. If y is NaN, return false.
        if lhs.is_nan() || rhs.is_nan() {
            return false;
        }
        // 3. If x is the same Number value as y, return true.
        // 4. If x is +0𝔽 and y is -0𝔽, return true.
        // 5. If x is -0𝔽 and y is +0𝔽, return true.
        // 6. Return false.
        return lhs.as_f64() == rhs.as_f64();
    }

    // 3. Return SameValueNonNumber(x, y).
    same_value_non_number(lhs, rhs)
}

// 7.2.14 IsLooselyEqual ( x, y ), https://tc39.es/ecma262/#sec-islooselyequal
pub fn is_loosely_equal(vm: &Vm, lhs: Value, rhs: Value) -> ThrowCompletionOr<bool> {
    // 1. If Type(x) is the same as Type(y), then
    if same_type_for_equality(lhs, rhs) {
        // a. Return IsStrictlyEqual(x, y).
        return Ok(is_strictly_equal(lhs, rhs));
    }

    // 2. If x is null and y is undefined, return true.
    // 3. If x is undefined and y is null, return true.
    if lhs.is_nullish() && rhs.is_nullish() {
        return Ok(true);
    }

    // 4. NOTE: This step is replaced in section B.3.6.2.
    // B.3.6.2 Changes to IsLooselyEqual, https://tc39.es/ecma262/#sec-IsHTMLDDA-internal-slot-aec
    // 4. Perform the following steps:
    // a. If Type(x) is Object and x has an [[IsHTMLDDA]] internal slot and y is either null or undefined, return true.
    if lhs.is_object() && rhs.is_nullish() {
        // OPTIMIZATION: We can return early here since non-HTMLDDA objects and nullish values are never equal.
        return Ok(lhs.as_object().is_htmldda());
    }

    // b. If x is either null or undefined and Type(y) is Object and y has an [[IsHTMLDDA]] internal slot, return true.
    if lhs.is_nullish() && rhs.is_object() {
        // OPTIMIZATION: We can return early here since non-HTMLDDA objects and nullish values are never equal.
        return Ok(rhs.as_object().is_htmldda());
    }

    // == End of B.3.6.2 ==

    // 5. If Type(x) is Number and Type(y) is String, return ! IsLooselyEqual(x, ! ToNumber(y)).
    if lhs.is_number() && rhs.is_string() {
        return is_loosely_equal(vm, lhs, rhs.to_number(vm).must());
    }

    // 6. If Type(x) is String and Type(y) is Number, return ! IsLooselyEqual(! ToNumber(x), y).
    if lhs.is_string() && rhs.is_number() {
        return is_loosely_equal(vm, lhs.to_number(vm).must(), rhs);
    }

    // 7. If Type(x) is BigInt and Type(y) is String, then
    if lhs.is_bigint() && rhs.is_string() {
        // a. Let n be StringToBigInt(y).
        let bigint = string_to_bigint(vm, &rhs.as_string());

        // b. If n is undefined, return false.
        let Some(bigint) = bigint else {
            return Ok(false);
        };

        // c. Return ! IsLooselyEqual(x, n).
        return is_loosely_equal(vm, lhs, Value::from_bigint(bigint));
    }

    // 8. If Type(x) is String and Type(y) is BigInt, return ! IsLooselyEqual(y, x).
    if lhs.is_string() && rhs.is_bigint() {
        return is_loosely_equal(vm, rhs, lhs);
    }

    // 9. If Type(x) is Boolean, return ! IsLooselyEqual(! ToNumber(x), y).
    if lhs.is_boolean() {
        return is_loosely_equal(vm, lhs.to_number(vm).must(), rhs);
    }

    // 10. If Type(y) is Boolean, return ! IsLooselyEqual(x, ! ToNumber(y)).
    if rhs.is_boolean() {
        return is_loosely_equal(vm, lhs, rhs.to_number(vm).must());
    }

    // 11. If Type(x) is either String, Number, BigInt, or Symbol and Type(y) is Object, return ! IsLooselyEqual(x, ? ToPrimitive(y)).
    if (lhs.is_string() || lhs.is_number() || lhs.is_bigint() || lhs.is_symbol()) && rhs.is_object() {
        let rhs_primitive = rhs.to_primitive(vm, PreferredType::Default)?;
        return is_loosely_equal(vm, lhs, rhs_primitive);
    }

    // 12. If Type(x) is Object and Type(y) is either String, Number, BigInt, or Symbol, return ! IsLooselyEqual(? ToPrimitive(x), y).
    if lhs.is_object() && (rhs.is_string() || rhs.is_number() || rhs.is_bigint() || rhs.is_symbol()) {
        let lhs_primitive = lhs.to_primitive(vm, PreferredType::Default)?;
        return is_loosely_equal(vm, lhs_primitive, rhs);
    }

    // 13. If Type(x) is BigInt and Type(y) is Number, or if Type(x) is Number and Type(y) is BigInt, then
    if (lhs.is_bigint() && rhs.is_number()) || (lhs.is_number() && rhs.is_bigint()) {
        // a. If x or y are any of NaN, +∞𝔽, or -∞𝔽, return false.
        if lhs.is_nan() || lhs.is_infinity() || rhs.is_nan() || rhs.is_infinity() {
            return Ok(false);
        }

        // b. If ℝ(x) = ℝ(y), return true; otherwise return false.
        if (lhs.is_number() && !lhs.is_integral_number()) || (rhs.is_number() && !rhs.is_integral_number()) {
            return Ok(false);
        }

        assert!(!lhs.is_nan() && !rhs.is_nan());

        let (number_side, bigint_side) = if lhs.is_number() { (lhs, rhs) } else { (rhs, lhs) };

        return Ok(
            big_int_algorithms::compare_to_double(bigint_side.as_bigint().big_integer(), number_side.as_f64())
                == CompareResult::DoubleEqualsBigInt,
        );
    }

    // 14. Return false.
    Ok(false)
}

// 7.2.13 IsLessThan ( x, y, LeftFirst ), https://tc39.es/ecma262/#sec-islessthan
pub fn is_less_than(vm: &Vm, lhs: Value, rhs: Value, left_first: bool) -> ThrowCompletionOr<TriState> {
    let x_primitive;
    let y_primitive;

    // 1. If the LeftFirst flag is true, then
    if left_first {
        // a. Let px be ? ToPrimitive(x, number).
        x_primitive = lhs.to_primitive(vm, PreferredType::Number)?;

        // b. Let py be ? ToPrimitive(y, number).
        y_primitive = rhs.to_primitive(vm, PreferredType::Number)?;
    } else {
        // a. NOTE: The order of evaluation needs to be reversed to preserve left to right evaluation.

        // b. Let py be ? ToPrimitive(y, number).
        y_primitive = lhs.to_primitive(vm, PreferredType::Number)?;

        // c. Let px be ? ToPrimitive(x, number).
        x_primitive = rhs.to_primitive(vm, PreferredType::Number)?;
    }

    // 3. If px is a String and py is a String, then
    if x_primitive.is_string() && y_primitive.is_string() {
        let x_string = x_primitive.as_string();
        let y_string = y_primitive.as_string();

        // a. Let lx be the length of px.
        // b. Let ly be the length of py.
        // c. For each integer i such that 0 ≤ i < min(lx, ly), in ascending order, do
        //     i. Let cx be the integer that is the numeric value of the code unit at index i within px.
        //     ii. Let cy be the integer that is the numeric value of the code unit at index i within py.
        //     iii. If cx < cy, return true.
        //     iv. If cx > cy, return false.
        // d. If lx < ly, return true. Otherwise, return false.
        let is_less = x_string
            .utf16_string_view()
            .is_code_unit_less_than(y_string.utf16_string_view());
        return Ok(if is_less { TriState::True } else { TriState::False });
    }

    // 4. Else,
    // a. If px is a BigInt and py is a String, then
    if x_primitive.is_bigint() && y_primitive.is_string() {
        // i. Let ny be StringToBigInt(py).
        let y_bigint = string_to_bigint(vm, &y_primitive.as_string());

        // ii. If ny is undefined, return undefined.
        let Some(y_bigint) = y_bigint else {
            return Ok(TriState::Unknown);
        };

        // iii. Return BigInt::lessThan(px, ny).
        if x_primitive.as_bigint().big_integer() < y_bigint.big_integer() {
            return Ok(TriState::True);
        }
        return Ok(TriState::False);
    }

    // b. If px is a String and py is a BigInt, then
    if x_primitive.is_string() && y_primitive.is_bigint() {
        // i. Let nx be StringToBigInt(px).
        let x_bigint = string_to_bigint(vm, &x_primitive.as_string());

        // ii. If nx is undefined, return undefined.
        let Some(x_bigint) = x_bigint else {
            return Ok(TriState::Unknown);
        };

        // iii. Return BigInt::lessThan(nx, py).
        if x_bigint.big_integer() < y_primitive.as_bigint().big_integer() {
            return Ok(TriState::True);
        }
        return Ok(TriState::False);
    }

    // c. NOTE: Because px and py are primitive values, evaluation order is not important.

    // d. Let nx be ? ToNumeric(px).
    let x_numeric = x_primitive.to_numeric(vm)?;

    // e. Let ny be ? ToNumeric(py).
    let y_numeric = y_primitive.to_numeric(vm)?;

    // h. If nx or ny is NaN, return undefined.
    if x_numeric.is_nan() || y_numeric.is_nan() {
        return Ok(TriState::Unknown);
    }

    // i. If nx is -∞𝔽 or ny is +∞𝔽, return true.
    if x_numeric.is_positive_infinity() || y_numeric.is_negative_infinity() {
        return Ok(TriState::False);
    }

    // j. If nx is +∞𝔽 or ny is -∞𝔽, return false.
    if x_numeric.is_negative_infinity() || y_numeric.is_positive_infinity() {
        return Ok(TriState::True);
    }

    // f. If Type(nx) is the same as Type(ny), then

    // i. If nx is a Number, then
    if x_numeric.is_number() && y_numeric.is_number() {
        // 1. Return Number::lessThan(nx, ny).
        if x_numeric.as_f64() < y_numeric.as_f64() {
            return Ok(TriState::True);
        }
        return Ok(TriState::False);
    }

    // ii. Else,
    if x_numeric.is_bigint() && y_numeric.is_bigint() {
        // 1. Assert: nx is a BigInt.
        // 2. Return BigInt::lessThan(nx, ny).
        if x_numeric.as_bigint().big_integer() < y_numeric.as_bigint().big_integer() {
            return Ok(TriState::True);
        }
        return Ok(TriState::False);
    }

    // g. Assert: nx is a BigInt and ny is a Number, or nx is a Number and ny is a BigInt.
    assert!((x_numeric.is_number() && y_numeric.is_bigint()) || (x_numeric.is_bigint() && y_numeric.is_number()));

    // k. If ℝ(nx) < ℝ(ny), return true; otherwise return false.
    assert!(!x_numeric.is_nan() && !y_numeric.is_nan());
    let x_lower_than_y = if x_numeric.is_number() {
        big_int_algorithms::compare_to_double(y_numeric.as_bigint().big_integer(), x_numeric.as_f64())
            == CompareResult::DoubleLessThanBigInt
    } else {
        big_int_algorithms::compare_to_double(x_numeric.as_bigint().big_integer(), y_numeric.as_f64())
            == CompareResult::DoubleGreaterThanBigInt
    };
    if x_lower_than_y {
        return Ok(TriState::True);
    }
    Ok(TriState::False)
}

/// Formats a value the way AK formats a JS::Value, without side effects.
impl Utf16Display for Value {
    fn fmt_utf16(&self, builder: &mut Utf16StringBuilder) {
        builder.append(Utf16View::of_string(&self.to_utf16_string_without_side_effects()));
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

impl NumberStringBuilder for Utf16StringBuilder {
    fn append_ascii(&mut self, text: &[u8]) {
        self.append_ascii_bytes(text);
    }

    fn append_repeated_ascii(&mut self, code_unit: u8, count: usize) {
        Utf16StringBuilder::append_repeated_ascii(self, code_unit, count);
    }
}

impl NumberStringBuilder for AsciiBuffer {
    fn append_ascii(&mut self, text: &[u8]) {
        let end = self.length + text.len();
        self.bytes[self.length..end].copy_from_slice(text);
        self.length = end;
    }

    fn append_repeated_ascii(&mut self, code_unit: u8, count: usize) {
        let end = self.length + count;
        self.bytes[self.length..end].fill(code_unit);
        self.length = end;
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

/// Number::toString(value, 10) as a string with ASCII storage, written on the stack first, where it always fits.
pub fn number_to_ascii_utf16_string(value: f64) -> Utf16String {
    let mut builder = AsciiBuffer::new();
    append_number_to_string(&mut builder, value);
    Utf16String::from_ascii(builder.as_bytes())
}

/// The decimal representation of an integer, as AK::Utf16String::number writes it.
pub fn integer_to_utf16_string(value: i64) -> Utf16String {
    Utf16String::from_ascii(DecimalDigits::new_signed(value).as_bytes())
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

    // OPTIMIZATION: For an integer of a magnitude below 2^53, the steps below produce its decimal digits, preceded by
    //               "-" if it is negative.
    if value.trunc() == value && value.abs() < 9_007_199_254_740_992.0 {
        builder.append_ascii(DecimalDigits::new_signed(value as i64).as_bytes());
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

struct AsciiBuffer {
    bytes: [u8; 32],
    length: usize,
}

impl AsciiBuffer {
    fn new() -> Self {
        Self {
            bytes: [0; 32],
            length: 0,
        }
    }

    fn as_bytes(&self) -> &[u8] {
        &self.bytes[..self.length]
    }
}

/// The decimal representation of an integer, written on the stack.
pub(crate) struct DecimalDigits {
    // The 20 digits of u64::MAX, or a minus sign and the 19 digits of i64::MIN.
    digits: [u8; 21],
    start: usize,
}

impl DecimalDigits {
    pub(crate) fn new(mut value: u64) -> Self {
        let mut digits = [0; 21];
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

    pub(crate) fn new_signed(value: i64) -> Self {
        let mut result = Self::new(value.unsigned_abs());
        if value < 0 {
            result.start -= 1;
            result.digits[result.start] = b'-';
        }
        result
    }

    pub(crate) fn as_bytes(&self) -> &[u8] {
        &self.digits[self.start..]
    }

    pub(crate) fn as_str(&self) -> &str {
        core::str::from_utf8(self.as_bytes()).expect("decimal digits are ASCII")
    }
}
