/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <AK/Utf16View.h>
#include <AK/Utf32View.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/StringConstructor.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/Utf16String.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

GC_DEFINE_ALLOCATOR(StringConstructor);

StringConstructor::StringConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.String.as_string(), realm.intrinsics().function_prototype())
{
}

void StringConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 22.1.2.3 String.prototype, https://tc39.es/ecma262/#sec-string.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().string_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.raw, raw, 1, attr);
    define_native_function(realm, vm.names.fromCharCode, from_char_code, 1, attr);
    define_native_function(realm, vm.names.fromCodePoint, from_code_point, 1, attr);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 22.1.1.1 String ( value ), https://tc39.es/ecma262/#sec-string-constructor-string-value
ThrowCompletionOr<Value> StringConstructor::call()
{
    auto& vm = this->vm();
    auto value = vm.argument(0);

    // 1. If value is not present, let s be the empty String.
    if (!vm.argument_count())
        return PrimitiveString::create(vm, String {});

    // 2. Else,
    // a. If NewTarget is undefined and value is a Symbol, return SymbolDescriptiveString(value).
    if (value.is_symbol())
        return PrimitiveString::create(vm, MUST(value.as_symbol().descriptive_string()));

    // b. Let s be ? ToString(value).
    // 3. If NewTarget is undefined, return s.
    return TRY(value.to_primitive_string(vm));
}

// 22.1.1.1 String ( value ), https://tc39.es/ecma262/#sec-string-constructor-string-value
ThrowCompletionOr<GC::Ref<Object>> StringConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();
    auto value = vm.argument(0);

    PrimitiveString* primitive_string;

    // 1. If value is not present, let s be the empty String.
    if (!vm.argument_count()) {
        primitive_string = PrimitiveString::create(vm, String {});
    }
    // 2. Else,
    else {
        // b. Let s be ? ToString(value).
        primitive_string = TRY(value.to_primitive_string(vm));
    }

    // 4. Return StringCreate(s, ? GetPrototypeFromConstructor(NewTarget, "%String.prototype%")).
    auto* prototype = TRY(get_prototype_from_constructor(vm, new_target, &Intrinsics::string_prototype));
    return StringObject::create(realm, *primitive_string, *prototype);
}

// 22.1.2.1 String.fromCharCode ( ...codeUnits ), https://tc39.es/ecma262/#sec-string.fromcharcode
JS_DEFINE_NATIVE_FUNCTION(StringConstructor::from_char_code)
{
    // 1. Let result be the empty String.
    Utf16Data string;
    string.ensure_capacity(vm.argument_count());

    // 2. For each element next of codeUnits, do
    for (size_t i = 0; i < vm.argument_count(); ++i) {
        // a. Let nextCU be the code unit whose numeric value is ℝ(? ToUint16(next)).
        auto next_code_unit = TRY(vm.argument(i).to_u16(vm));

        // b. Set result to the string-concatenation of result and nextCU.
        string.append(next_code_unit);
    }

    // 3. Return result.
    return PrimitiveString::create(vm, Utf16String::create(move(string)));
}

// 22.1.2.2 String.fromCodePoint ( ...codePoints ), https://tc39.es/ecma262/#sec-string.fromcodepoint
JS_DEFINE_NATIVE_FUNCTION(StringConstructor::from_code_point)
{
    // 1. Let result be the empty String.
    Utf16Data string;
    // This will be an under-estimate if any code point is > 0xffff.
    string.ensure_capacity(vm.argument_count());

    // 2. For each element next of codePoints, do
    for (size_t i = 0; i < vm.argument_count(); ++i) {
        // a. Let nextCP be ? ToNumber(next).
        auto next_code_point = TRY(vm.argument(i).to_number(vm));

        // b. If IsIntegralNumber(nextCP) is false, throw a RangeError exception.
        if (!next_code_point.is_integral_number())
            return vm.throw_completion<RangeError>(ErrorType::InvalidCodePoint, next_code_point.to_string_without_side_effects());

        auto code_point = MUST(next_code_point.to_i32(vm));

        // c. If ℝ(nextCP) < 0 or ℝ(nextCP) > 0x10FFFF, throw a RangeError exception.
        if (code_point < 0 || code_point > 0x10FFFF)
            return vm.throw_completion<RangeError>(ErrorType::InvalidCodePoint, next_code_point.to_string_without_side_effects());

        // d. Set result to the string-concatenation of result and UTF16EncodeCodePoint(ℝ(nextCP)).
        MUST(code_point_to_utf16(string, static_cast<u32>(code_point)));
    }

    // 3. Assert: If codePoints is empty, then result is the empty String.
    if (!vm.argument_count())
        VERIFY(string.is_empty());

    // 4. Return result.
    return PrimitiveString::create(vm, Utf16String::create(move(string)));
}

// 22.1.2.4 String.raw ( template, ...substitutions ), https://tc39.es/ecma262/#sec-string.raw
JS_DEFINE_NATIVE_FUNCTION(StringConstructor::raw)
{
    auto template_ = vm.argument(0);

    // 1. Let substitutionCount be the number of elements in substitutions.
    auto substitution_count = vm.argument_count() > 0 ? vm.argument_count() - 1 : 0;

    // 2. Let cooked be ? ToObject(template).
    auto cooked = TRY(template_.to_object(vm));

    // 3. Let literals be ? ToObject(? Get(cooked, "raw")).
    auto literals = TRY(TRY(cooked->get(vm.names.raw)).to_object(vm));

    // 4. Let literalCount be ? LengthOfArrayLike(literals).
    auto literal_count = TRY(length_of_array_like(vm, literals));

    // 5. If literalCount ≤ 0, return the empty String.
    if (literal_count == 0)
        return PrimitiveString::create(vm, String {});

    // 6. Let R be the empty String.
    StringBuilder builder;

    // 7. Let nextIndex be 0.
    // 8. Repeat,
    for (size_t i = 0; i < literal_count; ++i) {
        // a. Let nextLiteralVal be ? Get(literals, ! ToString(𝔽(nextIndex))).
        auto next_literal_value = TRY(literals->get(PropertyKey(i)));

        // b. Let nextLiteral be ? ToString(nextLiteralVal).
        auto next_literal = TRY(next_literal_value.to_string(vm));

        // c. Set R to the string-concatenation of R and nextLiteral.
        builder.append(next_literal);

        // d. If nextIndex + 1 = literalCount, return R.
        if (i + 1 == literal_count)
            break;

        // e. If nextIndex < substitutionCount, then
        if (i < substitution_count) {
            // i. Let nextSubVal be substitutions[nextIndex].
            auto next_substitution_value = vm.argument(i + 1);

            // ii. Let nextSub be ? ToString(nextSubVal).
            auto next_substitution = TRY(next_substitution_value.to_string(vm));

            // iii. Set R to the string-concatenation of R and nextSub.
            builder.append(next_substitution);
        }

        // f. Set nextIndex to nextIndex + 1.
    }
    return PrimitiveString::create(vm, builder.to_byte_string());
}

}
