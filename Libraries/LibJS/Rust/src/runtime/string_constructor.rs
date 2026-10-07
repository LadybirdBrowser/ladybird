/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_abi::Builtin;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{get_prototype_from_constructor, length_of_array_like};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::string_object::StringObject;
use crate::utf16::{Utf16StringBuilder, Utf16View};

#[repr(C)]
#[derive(Trace)]
pub struct StringConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    StringConstructor,
    initialize: StringConstructor::initialize,
    call: StringConstructor::call,
    construct: StringConstructor::construct
);

impl StringConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<StringConstructor> {
        realm.create_object(
            vm,
            StringConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.String.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 22.1.2.3 String.prototype, https://tc39.es/ecma262/#sec-string.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().string_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.raw,
            raw_native!(StringConstructor::raw),
            1,
            attributes,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.fromCharCode,
            raw_native!(StringConstructor::from_char_code),
            1,
            attributes,
            Some(Builtin::StringFromCharCode),
        );
        object.define_native_function(
            vm,
            realm,
            &names.fromCodePoint,
            raw_native!(StringConstructor::from_code_point),
            1,
            attributes,
            None,
        );

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 22.1.1.1 String ( value ), https://tc39.es/ecma262/#sec-string-constructor-string-value
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. If value is not present, let s be the empty String.
        if vm.argument_count() == 0 {
            return Ok(Value::from_string(PrimitiveString::create(vm, Utf16String::default())));
        }

        // 2. Else,
        // a. If NewTarget is undefined and value is a Symbol, return SymbolDescriptiveString(value).
        if value.is_symbol() {
            return Ok(Value::from_string(PrimitiveString::create(
                vm,
                value.as_symbol().descriptive_string(),
            )));
        }

        // b. Let s be ? ToString(value).
        // 3. If NewTarget is undefined, return s.
        Ok(Value::from_string(value.to_primitive_string(vm)?))
    }

    // 22.1.1.1 String ( value ), https://tc39.es/ecma262/#sec-string-constructor-string-value
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let value = vm.argument(0);

        // 1. If value is not present, let s be the empty String.
        let primitive_string = if vm.argument_count() == 0 {
            PrimitiveString::create(vm, Utf16String::default())
        }
        // 2. Else,
        else {
            // b. Let s be ? ToString(value).
            value.to_primitive_string(vm)?
        };

        // 4. Return StringCreate(s, ? GetPrototypeFromConstructor(NewTarget, "%String.prototype%")).
        let prototype = get_prototype_from_constructor(vm, new_target, Intrinsics::string_prototype)?;
        Ok(StringObject::create(vm, realm, primitive_string, prototype).upcast())
    }

    // 22.1.2.1 String.fromCharCode ( ...codeUnits ), https://tc39.es/ecma262/#sec-string.fromcharcode
    fn from_char_code(vm: &Vm) -> ThrowCompletionOr<Value> {
        if vm.argument_count() == 1 {
            return from_char_code_impl(vm, vm.argument(0));
        }

        // 1. Let result be the empty String.
        let mut builder = Utf16StringBuilder::with_capacity(vm.argument_count());

        // 2. For each element next of codeUnits, do
        for i in 0..vm.argument_count() {
            // a. Let nextCU be the code unit whose numeric value is ℝ(? ToUint16(next)).
            let next_code_unit = vm.argument(i).to_u16(vm)?;

            // b. Set result to the string-concatenation of result and nextCU.
            builder.append_code_unit(next_code_unit);
        }

        // 3. Return result.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            builder.to_utf16_string(),
        )))
    }

    // 22.1.2.2 String.fromCodePoint ( ...codePoints ), https://tc39.es/ecma262/#sec-string.fromcodepoint
    fn from_code_point(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let result be the empty String.
        // NOTE: This will be an under-estimate if any code point is > 0xffff.
        let mut builder = Utf16StringBuilder::with_capacity(vm.argument_count());

        // 2. For each element next of codePoints, do
        for i in 0..vm.argument_count() {
            // a. Let nextCP be ? ToNumber(next).
            let next_code_point = vm.argument(i).to_number(vm)?;

            // b. If IsIntegralNumber(nextCP) is false, throw a RangeError exception.
            if !next_code_point.is_integral_number() {
                return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidCodePoint, &[&next_code_point]);
            }

            let code_point = next_code_point.to_i32(vm).must();

            // c. If ℝ(nextCP) < 0 or ℝ(nextCP) > 0x10FFFF, throw a RangeError exception.
            if !(0..=0x10FFFF).contains(&code_point) {
                return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidCodePoint, &[&next_code_point]);
            }

            // d. Set result to the string-concatenation of result and UTF16EncodeCodePoint(ℝ(nextCP)).
            builder.append_code_point(code_point as u32);
        }

        // 3. Assert: If codePoints is empty, then result is the empty String.
        if vm.argument_count() == 0 {
            assert!(builder.is_empty());
        }

        // 4. Return result.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            builder.to_utf16_string(),
        )))
    }

    // 22.1.2.4 String.raw ( template, ...substitutions ), https://tc39.es/ecma262/#sec-string.raw
    fn raw(vm: &Vm) -> ThrowCompletionOr<Value> {
        let template = vm.argument(0);

        // 1. Let substitutionCount be the number of elements in substitutions.
        let substitution_count = vm.argument_count().saturating_sub(1);

        // 2. Let cooked be ? ToObject(template).
        let cooked = template.to_object(vm)?;

        // 3. Let literals be ? ToObject(? Get(cooked, "raw")).
        let literals = cooked.get(vm, &vm.names.raw)?.to_object(vm)?;

        // 4. Let literalCount be ? LengthOfArrayLike(literals).
        let literal_count = length_of_array_like(vm, &literals)?;

        // 5. If literalCount ≤ 0, return the empty String.
        if literal_count == 0 {
            return Ok(Value::from_string(PrimitiveString::create(vm, Utf16String::default())));
        }

        // 6. Let R be the empty String.
        let mut builder = Utf16StringBuilder::new();

        // 7. Let nextIndex be 0.
        // 8. Repeat,
        for i in 0..literal_count {
            // a. Let nextLiteralVal be ? Get(literals, ! ToString(𝔽(nextIndex))).
            let next_literal_value = literals.get(vm, &PropertyKey::from_number(i))?;

            // b. Let nextLiteral be ? ToString(nextLiteralVal).
            let next_literal = next_literal_value.to_utf16_string(vm)?;

            // c. Set R to the string-concatenation of R and nextLiteral.
            builder.append(Utf16View::of_string(&next_literal));

            // d. If nextIndex + 1 = literalCount, return R.
            if i + 1 == literal_count {
                break;
            }

            // e. If nextIndex < substitutionCount, then
            if i < substitution_count as u64 {
                // i. Let nextSubVal be substitutions[nextIndex].
                let next_substitution_value = vm.argument(i as usize + 1);

                // ii. Let nextSub be ? ToString(nextSubVal).
                let next_substitution = next_substitution_value.to_utf16_string(vm)?;

                // iii. Set R to the string-concatenation of R and nextSub.
                builder.append(Utf16View::of_string(&next_substitution));
            }

            // f. Set nextIndex to nextIndex + 1.
        }
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            builder.to_utf16_string(),
        )))
    }
}

// 22.1.2.1 String.fromCharCode ( ...codeUnits ), https://tc39.es/ecma262/#sec-string.fromcharcode
pub fn from_char_code_impl(vm: &Vm, code_unit: Value) -> ThrowCompletionOr<Value> {
    let value = code_unit.to_u16(vm)?;
    Ok(Value::from_string(PrimitiveString::create_from_utf16_view(
        vm,
        Utf16View::Utf16(&[value]),
    )))
}
