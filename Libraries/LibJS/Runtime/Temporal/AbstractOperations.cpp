/*
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>

namespace JS::Temporal {

// 14.4.1.1 GetOptionsObject ( options ), https://tc39.es/proposal-temporal/#sec-getoptionsobject
ThrowCompletionOr<GC::Ref<Object>> get_options_object(VM& vm, Value options)
{
    auto& realm = *vm.current_realm();

    // 1. If options is undefined, then
    if (options.is_undefined()) {
        // a. Return OrdinaryObjectCreate(null).
        return Object::create(realm, nullptr);
    }

    // 2. If options is an Object, then
    if (options.is_object()) {
        // a. Return options.
        return options.as_object();
    }

    // 3. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::NotAnObject, "Options");
}

// 14.4.1.2 GetOption ( options, property, type, values, default ), https://tc39.es/proposal-temporal/#sec-getoption
ThrowCompletionOr<Value> get_option(VM& vm, Object const& options, PropertyKey const& property, OptionType type, ReadonlySpan<StringView> values, OptionDefault const& default_)
{
    VERIFY(property.is_string());

    // 1. Let value be ? Get(options, property).
    auto value = TRY(options.get(property));

    // 2. If value is undefined, then
    if (value.is_undefined()) {
        // a. If default is REQUIRED, throw a RangeError exception.
        if (default_.has<DefaultRequired>())
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, "undefined"sv, property.as_string());

        // b. Return default.
        return default_.visit(
            [](DefaultRequired) -> Value { VERIFY_NOT_REACHED(); },
            [](Empty) -> Value { return js_undefined(); },
            [](bool default_) -> Value { return Value { default_ }; },
            [](double default_) -> Value { return Value { default_ }; },
            [&](StringView default_) -> Value { return PrimitiveString::create(vm, default_); });
    }

    // 3. If type is BOOLEAN, then
    if (type == OptionType::Boolean) {
        // a. Set value to ToBoolean(value).
        value = Value { value.to_boolean() };
    }
    // 4. Else,
    else {
        // a. Assert: type is STRING.
        VERIFY(type == OptionType::String);

        // b. Set value to ? ToString(value).
        value = TRY(value.to_primitive_string(vm));
    }

    // 5. If values is not EMPTY and values does not contain value, throw a RangeError exception.
    if (!values.is_empty()) {
        // NOTE: Every location in the spec that invokes GetOption with type=boolean also has values=undefined.
        VERIFY(value.is_string());

        if (auto value_string = value.as_string().utf8_string(); !values.contains_slow(value_string))
            return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, value_string, property.as_string());
    }

    // 6. Return value.
    return value;
}

}
