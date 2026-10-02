/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::construct;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::mathematical_value::MathematicalValue;
use crate::runtime::intl::number_format::{NumberFormat, format_numeric};
use crate::runtime::native_function::raw_native;
use crate::runtime::number_object::NumberObject;
use crate::runtime::number_prototype_algorithms;
use crate::runtime::object::{ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;

/// %Number.prototype%, which is a Number object whose [[NumberData]] is +0𝔽.
#[repr(C)]
#[derive(Trace)]
pub struct NumberPrototype {
    base: NumberObject,
}

define_object_class!(NumberPrototype, extends: [NumberObject, Object], methods: {
    initialize: NumberPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

// thisNumberValue ( value ), https://tc39.es/ecma262/#thisnumbervalue
fn this_number_value(vm: &Vm, value: Value) -> ThrowCompletionOr<Value> {
    // 1. If Type(value) is Number, return value.
    if value.is_number() {
        return Ok(value);
    }

    // 2. If Type(value) is Object and value has a [[NumberData]] internal slot, then
    if value.is_object()
        && let Some(number) = value.as_object().downcast::<NumberObject>()
    {
        // a. Let n be value.[[NumberData]].
        // b. Assert: Type(n) is Number.
        // c. Return n.
        return Ok(Value::from_f64(number.number()));
    }

    // 3. Throw a TypeError exception.
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"Number"])
}

/// Number::toString(x) as a string value, which cannot throw for a Number.
fn number_to_string_value(vm: &Vm, number_value: Value) -> Value {
    Value::from_string(PrimitiveString::create(vm, number_value.to_utf16_string(vm).must()))
}

impl NumberPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<NumberPrototype> {
        realm.create_object(
            vm,
            NumberPrototype {
                base: NumberObject::new(vm, Self::CLASS, 0.0, realm.object_prototype()),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE);
        object.define_native_function(
            vm,
            realm,
            &names.toExponential,
            raw_native!(NumberPrototype::to_exponential),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.toFixed,
            raw_native!(NumberPrototype::to_fixed),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.toLocaleString,
            raw_native!(NumberPrototype::to_locale_string),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.toPrecision,
            raw_native!(NumberPrototype::to_precision),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.toString,
            raw_native!(NumberPrototype::to_string),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.valueOf,
            raw_native!(NumberPrototype::value_of),
            0,
            attr,
            None,
        );
    }

    // 21.1.3.2 Number.prototype.toExponential ( fractionDigits ), https://tc39.es/ecma262/#sec-number.prototype.toexponential
    fn to_exponential(vm: &Vm) -> ThrowCompletionOr<Value> {
        let fraction_digits_value = vm.argument(0);

        // 1. Let x be ? thisNumberValue(this value).
        let number_value = this_number_value(vm, vm.this_value())?;

        // 2. Let f be ? ToIntegerOrInfinity(fractionDigits).
        let fraction_digits = fraction_digits_value.to_integer_or_infinity(vm)?;

        // 3. Assert: If fractionDigits is undefined, then f is 0.
        assert!(!fraction_digits_value.is_undefined() || fraction_digits == 0.0);

        // 4. If x is not finite, return Number::toString(x).
        if !number_value.is_finite_number() {
            return Ok(number_to_string_value(vm, number_value));
        }

        // 5. If f < 0 or f > 100, throw a RangeError exception.
        if !(0.0..=100.0).contains(&fraction_digits) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidFractionDigits, &[]);
        }

        // 6-15. NB: number_prototype_algorithms::to_exponential() performs these steps.
        let fraction_digits = (!fraction_digits_value.is_undefined()).then_some(fraction_digits as u32);
        let result = number_prototype_algorithms::to_exponential(number_value.as_f64(), fraction_digits);
        Ok(Value::from_string(PrimitiveString::create_from_utf8(vm, &result)))
    }

    // 21.1.3.3 Number.prototype.toFixed ( fractionDigits ), https://tc39.es/ecma262/#sec-number.prototype.tofixed
    fn to_fixed(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let x be ? thisNumberValue(this value).
        let number_value = this_number_value(vm, vm.this_value())?;

        // 2. Let f be ? ToIntegerOrInfinity(fractionDigits).
        // 3. Assert: If fractionDigits is undefined, then f is 0.
        let fraction_digits = vm.argument(0).to_integer_or_infinity(vm)?;

        // 4. If f is not finite, throw a RangeError exception.
        if !fraction_digits.is_finite() {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidFractionDigits, &[]);
        }

        // 5. If f < 0 or f > 100, throw a RangeError exception.
        if !(0.0..=100.0).contains(&fraction_digits) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidFractionDigits, &[]);
        }

        // 6. If x is not finite, return Number::toString(x).
        if !number_value.is_finite_number() {
            return Ok(Value::from_string(PrimitiveString::create(
                vm,
                number_value.to_utf16_string(vm)?,
            )));
        }

        // 7-12. NB: number_prototype_algorithms::to_fixed() performs these steps, except for step 10, where x ≥ 10^21
        //       and m is ! ToString(𝔽(x)).
        match number_prototype_algorithms::to_fixed(number_value.as_f64(), fraction_digits as u32) {
            Some(result) => Ok(Value::from_string(PrimitiveString::create_from_utf8(vm, &result))),
            None => Ok(number_to_string_value(vm, number_value)),
        }
    }

    // 20.2.1 Number.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-number.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let x be ? thisNumberValue(this value).
        let number_value = this_number_value(vm, vm.this_value())?;

        // 2. Let numberFormat be ? Construct(%NumberFormat%, « locales, options »).
        let number_format = construct(
            vm,
            realm.intrinsics().intl_number_format_constructor(vm),
            &[locales, options],
            None,
        )?
        .downcast::<NumberFormat>()
        .expect("the Intl.NumberFormat constructor creates an Intl.NumberFormat");

        // 3. Return ? FormatNumeric(numberFormat, x).
        let formatted = format_numeric(&number_format, &MathematicalValue::from_value(number_value));
        Ok(Value::from_string(PrimitiveString::create(vm, formatted)))
    }

    // 21.1.3.5 Number.prototype.toPrecision ( precision ), https://tc39.es/ecma262/#sec-number.prototype.toprecision
    fn to_precision(vm: &Vm) -> ThrowCompletionOr<Value> {
        let precision_value = vm.argument(0);

        // 1. Let x be ? thisNumberValue(this value).
        let number_value = this_number_value(vm, vm.this_value())?;

        // 2. If precision is undefined, return ! ToString(x).
        if precision_value.is_undefined() {
            return Ok(number_to_string_value(vm, number_value));
        }

        // 3. Let p be ? ToIntegerOrInfinity(precision).
        let precision = precision_value.to_integer_or_infinity(vm)?;

        // 4. If x is not finite, return Number::toString(x).
        if !number_value.is_finite_number() {
            return Ok(number_to_string_value(vm, number_value));
        }

        // 5. If p < 1 or p > 100, throw a RangeError exception.
        if !(1.0..=100.0).contains(&precision) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidPrecision, &[]);
        }

        // 6-14. NB: number_prototype_algorithms::to_precision() performs these steps.
        let result = number_prototype_algorithms::to_precision(number_value.as_f64(), precision as u32);
        Ok(Value::from_string(PrimitiveString::create_from_utf8(vm, &result)))
    }

    // 21.1.3.6 Number.prototype.toString ( [ radix ] ), https://tc39.es/ecma262/#sec-number.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let x be ? thisNumberValue(this value).
        let number_value = this_number_value(vm, vm.this_value())?;

        // 2. If radix is undefined, let radixMV be 10.
        // 3. Else, let radixMV be ? ToIntegerOrInfinity(radix).
        let radix_mv = if vm.argument(0).is_undefined() {
            10.0
        } else {
            vm.argument(0).to_integer_or_infinity(vm)?
        };

        // 4. If radixMV < 2 or radixMV > 36, throw a RangeError exception.
        if !(2.0..=36.0).contains(&radix_mv) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidRadix, &[]);
        }

        // 5. If radixMV = 10, return ! ToString(x).
        if radix_mv == 10.0 {
            return Ok(number_to_string_value(vm, number_value));
        }

        // 6. Return the String representation of this Number value using the radix specified by radixMV. Letters a-z are used for digits with values 10 through 35. The precise algorithm is implementation-defined, however the algorithm should be a generalization of that specified in 6.1.6.1.20.
        let result = number_prototype_algorithms::to_string_with_radix(number_value.as_f64(), radix_mv as u32);
        Ok(Value::from_string(PrimitiveString::create_from_utf8(vm, &result)))
    }

    // 21.1.3.7 Number.prototype.valueOf ( ), https://tc39.es/ecma262/#sec-number.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? thisNumberValue(this value).
        this_number_value(vm, vm.this_value())
    }
}
