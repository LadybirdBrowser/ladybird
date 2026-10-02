/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::construct;
use crate::runtime::big_int::BigInt;
use crate::runtime::big_int_algorithms;
use crate::runtime::big_int_object::BigIntObject;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

/// %BigInt.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct BigIntPrototype {
    base: Object,
}

define_object_class!(BigIntPrototype, extends: [Object], methods: {
    initialize: BigIntPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

// thisBigIntValue ( value ), https://tc39.es/ecma262/#thisbigintvalue
fn this_bigint_value(vm: &Vm, value: Value) -> ThrowCompletionOr<Gc<BigInt>> {
    // 1. If value is a BigInt, return value.
    if value.is_bigint() {
        return Ok(value.as_bigint());
    }

    // 2. If value is an Object and value has a [[BigIntData]] internal slot, then
    if value.is_object()
        && let Some(bigint) = value.as_object().downcast::<BigIntObject>()
    {
        // a. Assert: value.[[BigIntData]] is a BigInt.
        // b. Return value.[[BigIntData]].
        return Ok(bigint.bigint());
    }

    // 3. Throw a TypeError exception.
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"BigInt"])
}

impl BigIntPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<BigIntPrototype> {
        realm.create_object(
            vm,
            BigIntPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.toString,
            raw_native!(BigIntPrototype::to_string),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.toLocaleString,
            raw_native!(BigIntPrototype::to_locale_string),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.valueOf,
            raw_native!(BigIntPrototype::value_of),
            0,
            attr,
            None,
        );

        // 21.2.3.5 BigInt.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-bigint.prototype-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(vm, names.BigInt.as_string())),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 21.2.3.3 BigInt.prototype.toString ( [ radix ] ), https://tc39.es/ecma262/#sec-bigint.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let x be ? thisBigIntValue(this value).
        let bigint = this_bigint_value(vm, vm.this_value())?;

        // 2. If radix is undefined, let radixMV be 10.
        let mut radix = 10.0;

        // 3. Else, let radixMV be ? ToIntegerOrInfinity(radix).
        if !vm.argument(0).is_undefined() {
            radix = vm.argument(0).to_integer_or_infinity(vm)?;

            // 4. If radixMV is not in the inclusive interval from 2 to 36, throw a RangeError exception.
            if !(2.0..=36.0).contains(&radix) {
                return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidRadix, &[]);
            }
        }

        // 5. Return BigInt::toString(x, radixMV).
        Ok(Value::from_string(PrimitiveString::create_from_utf8(
            vm,
            &big_int_algorithms::to_base(bigint.big_integer(), radix as u32),
        )))
    }

    // 21.2.3.2 BigInt.prototype.toLocaleString ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-bigint.prototype.tolocalestring
    // 20.3.1 BigInt.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-bigint.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let x be ? thisBigIntValue(this value).
        let _bigint = this_bigint_value(vm, vm.this_value())?;

        // 2. Let numberFormat be ? Construct(%NumberFormat%, « locales, options »).
        let _number_format = construct(
            vm,
            realm.intrinsics().intl_number_format_constructor(vm),
            &[locales, options],
            None,
        )?;

        // 3. Return ? FormatNumeric(numberFormat, x).
        unimplemented_runtime_function("Intl::format_numeric, for BigInt.prototype.toLocaleString", 0)
    }

    // 21.2.3.4 BigInt.prototype.valueOf ( ), https://tc39.es/ecma262/#sec-bigint.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? thisBigIntValue(this value).
        Ok(Value::from_bigint(this_bigint_value(vm, vm.this_value())?))
    }
}
