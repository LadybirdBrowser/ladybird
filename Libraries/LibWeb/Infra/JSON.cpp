/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Infra/JSON.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Infra {

// https://infra.spec.whatwg.org/#parse-a-json-string-to-a-javascript-value
WebIDL::ExceptionOr<JS::Value> parse_json_string_to_javascript_value(JS::Realm& realm, StringView string)
{
    auto& vm = realm.vm();

    // 1. Return ? Call(%JSON.parse%, undefined, « string »).
    return TRY(JS::call(vm, *realm.intrinsics().json_parse_function(), JS::js_undefined(), JS::PrimitiveString::create(vm, string)));
}

// https://infra.spec.whatwg.org/#parse-json-bytes-to-a-javascript-value
WebIDL::ExceptionOr<JS::Value> parse_json_bytes_to_javascript_value(JS::Realm& realm, ReadonlyBytes bytes)
{
    auto& vm = realm.vm();

    // 1. Let string be the result of running UTF-8 decode on bytes.
    TextCodec::UTF8Decoder decoder;
    auto string = TRY_OR_THROW_OOM(vm, decoder.to_utf8(bytes));

    // 2. Return the result of parsing a JSON string to an Infra value given string.
    return parse_json_string_to_javascript_value(realm, string);
}

// https://infra.spec.whatwg.org/#serialize-a-javascript-value-to-a-json-string
WebIDL::ExceptionOr<String> serialize_javascript_value_to_json_string(JS::VM& vm, JS::Value value)
{
    auto& realm = *vm.current_realm();

    // 1. Let result be ? Call(%JSON.stringify%, undefined, « value »).
    auto result = TRY(JS::call(vm, *realm.intrinsics().json_stringify_function(), JS::js_undefined(), value));

    // 2. If result is undefined, then throw a TypeError.
    if (result.is_undefined())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Result of stringifying value must not be undefined"sv };

    // 3. Assert: result is a string.
    VERIFY(result.is_string());

    // 4. Return result.
    return result.as_string().utf8_string();
}

// https://infra.spec.whatwg.org/#serialize-a-javascript-value-to-json-bytes
WebIDL::ExceptionOr<ByteBuffer> serialize_javascript_value_to_json_bytes(JS::VM& vm, JS::Value value)
{
    // 1. Let string be the result of serializing a JavaScript value to a JSON string given value.
    auto string = TRY(serialize_javascript_value_to_json_string(vm, value));

    // 2. Return the result of running UTF-8 encode on string.
    // NOTE: LibJS strings are stored as UTF-8.
    return TRY_OR_THROW_OOM(vm, ByteBuffer::copy(string.bytes()));
}

// https://infra.spec.whatwg.org/#convert-an-infra-value-to-a-json-compatible-javascript-value
[[nodiscard]] static JS::Value convert_an_infra_value_to_a_json_compatible_javascript_value(JS::Realm& realm, JSONTopLevel const& value)
{
    auto& vm = realm.vm();

    if (value.has<JSONValue>()) {
        // 1. If value is a string, boolean, number, or null, then return value.
        auto const& json_value = value.get<JSONValue>();

        if (json_value.has<JSONBaseValue>()) {
            auto const& base_value = json_value.get<JSONBaseValue>();

            if (base_value.has<String>())
                return JS::PrimitiveString::create(vm, base_value.get<String>());

            if (base_value.has<bool>())
                return JS::Value(base_value.get<bool>());

            if (base_value.has<u16>())
                return JS::Value(base_value.get<u16>());

            if (base_value.has<u32>())
                return JS::Value(base_value.get<u32>());

            VERIFY(base_value.has<Empty>());
            return JS::js_null();
        }

        // 2. If value is a list, then:
        VERIFY(json_value.has<Vector<JSONBaseValue>>());
        auto const& list_value = json_value.get<Vector<JSONBaseValue>>();

        // 1. Let jsValue be ! ArrayCreate(0).
        auto js_value = MUST(JS::Array::create(realm, 0));

        // 2. Let i be 0.
        u64 index = 0;

        // 3. For each listItem of value:
        for (auto const& list_item : list_value) {
            // 1. Let listItemJSValue be the result of converting an Infra value to a JSON-compatible JavaScript value,
            //    given listItem.
            auto list_item_js_value = convert_an_infra_value_to_a_json_compatible_javascript_value(realm, JSONValue { list_item });

            // 2. Perform ! CreateDataPropertyOrThrow(jsValue, ! ToString(i), listItemJSValue).
            MUST(js_value->create_data_property_or_throw(index, list_item_js_value));

            // 3. Set i to i + 1.
            ++index;
        }

        // 4. Return jsValue.
        return js_value;
    }

    // 3. Assert: value is a map.
    VERIFY(value.has<JSONObject>());
    auto const& map_value = value.get<JSONObject>();

    // 4. Let jsValue be ! OrdinaryObjectCreate(null).
    auto js_value = JS::Object::create(realm, nullptr);

    // 5. For each mapKey → mapValue of value:
    for (auto const& map_entry : map_value.value) {
        // 1. Assert: mapKey is a string.
        // 2. Let mapValueJSValue be the result of converting an Infra value to a JSON-compatible JavaScript value,
        //    given mapValue.
        auto map_value_js_value = convert_an_infra_value_to_a_json_compatible_javascript_value(realm, map_entry.value);

        // 3. Perform ! CreateDataPropertyOrThrow(jsValue, mapKey, mapValueJSValue).
        MUST(js_value->create_data_property_or_throw(map_entry.key, map_value_js_value));
    }

    // 6. Return jsValue.
    return js_value;
}

// https://infra.spec.whatwg.org/#serialize-an-infra-value-to-a-json-string
String serialize_an_infra_value_to_a_json_string(JS::Realm& realm, JSONTopLevel const& value)
{
    auto& vm = realm.vm();

    // 1. Let jsValue be the result of converting an Infra value to a JSON-compatible JavaScript value, given value.
    auto js_value = convert_an_infra_value_to_a_json_compatible_javascript_value(realm, value);

    // 2. Return ! Call(%JSON.stringify%, undefined, « jsValue »).
    // Spec Note: Since no additional arguments are passed to %JSON.stringify%, the resulting string will have no
    //            whitespace inserted.
    auto result = MUST(JS::call(vm, *realm.intrinsics().json_stringify_function(), JS::js_undefined(), js_value));
    VERIFY(result.is_string());
    return result.as_string().utf8_string();
}

// https://infra.spec.whatwg.org/#serialize-a-javascript-value-to-json-bytes
ByteBuffer serialize_an_infra_value_to_json_bytes(JS::Realm& realm, JSONTopLevel const& value)
{
    // 1. Let string be the result of serializing an Infra value to a JSON string, given value.
    auto string = serialize_an_infra_value_to_a_json_string(realm, value);

    // 2. Return the result of running UTF-8 encode on string. [ENCODING]
    // NOTE: LibJS strings are stored as UTF-8.
    return MUST(ByteBuffer::copy(string.bytes()));
}

}
