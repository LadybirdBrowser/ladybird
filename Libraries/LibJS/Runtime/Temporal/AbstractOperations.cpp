/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>

namespace JS::Temporal {

// 13.2 GetOptionsObject ( options ), https://tc39.es/proposal-temporal/#sec-getoptionsobject
ThrowCompletionOr<Object*> get_options_object(VM& vm, Value options)
{
    auto& realm = *vm.current_realm();

    // 1. If options is undefined, then
    if (options.is_undefined()) {
        // a. Return OrdinaryObjectCreate(null).
        return Object::create(realm, nullptr).ptr();
    }

    // 2. If Type(options) is Object, then
    if (options.is_object()) {
        // a. Return options.
        return &options.as_object();
    }

    // 3. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::NotAnObject, "Options");
}

// 13.3 GetOption ( options, property, type, values, fallback ), https://tc39.es/proposal-temporal/#sec-getoption
ThrowCompletionOr<Value> get_option(VM& vm, Object const& options, PropertyKey const& property, OptionType type, ReadonlySpan<StringView> values, OptionDefault const& default_)
{
    VERIFY(property.is_string());

    // 1. Let value be ? Get(options, property).
    auto value = TRY(options.get(property));

    // 2. If value is undefined, then
    if (value.is_undefined()) {
        // a. If default is required, throw a RangeError exception.
        if (default_.has<GetOptionRequired>())
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, "undefined"sv, property.as_string());

        // b. Return default.
        return default_.visit(
            [](GetOptionRequired) -> ThrowCompletionOr<Value> { VERIFY_NOT_REACHED(); },
            [](Empty) -> ThrowCompletionOr<Value> { return js_undefined(); },
            [](bool b) -> ThrowCompletionOr<Value> { return Value(b); },
            [](double d) -> ThrowCompletionOr<Value> { return Value(d); },
            [&vm](StringView s) -> ThrowCompletionOr<Value> { return PrimitiveString::create(vm, s); });
    }

    // 5. If type is "boolean", then
    if (type == OptionType::Boolean) {
        // a. Set value to ToBoolean(value).
        value = Value(value.to_boolean());
    }
    // 6. Else if type is "number", then
    else if (type == OptionType::Number) {
        // a. Set value to ? ToNumber(value).
        value = TRY(value.to_number(vm));

        // b. If value is NaN, throw a RangeError exception.
        if (value.is_nan())
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, vm.names.NaN.as_string(), property.as_string());
    }
    // 7. Else,
    else {
        // a. Assert: type is "string".
        VERIFY(type == OptionType::String);

        // b. Set value to ? ToString(value).
        value = TRY(value.to_primitive_string(vm));
    }

    // 8. If values is not undefined and values does not contain an element equal to value, throw a RangeError exception.
    if (!values.is_empty()) {
        // NOTE: Every location in the spec that invokes GetOption with type=boolean also has values=undefined.
        VERIFY(value.is_string());
        if (auto value_string = value.as_string().utf8_string(); !values.contains_slow(value_string))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, value_string, property.as_string());
    }

    // 9. Return value.
    return value;
}

}
