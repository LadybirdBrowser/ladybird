/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibJS/Runtime/AbstractOperations.h>
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

}
