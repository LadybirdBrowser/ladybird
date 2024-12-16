/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Crypto/CryptoBindings.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Bindings {

#define JWK_PARSE_STRING_PROPERTY(name)                                                     \
    if (json_object.has_string(#name##sv)) {                                                \
        key.name = MUST(String::from_byte_string(*json_object.get_byte_string(#name##sv))); \
    }

JS::ThrowCompletionOr<JsonWebKey> JsonWebKey::parse(JS::Realm& realm, ReadonlyBytes data)
{
    auto& vm = realm.vm();

    // 1. Let data be the sequence of bytes to be parsed.

    // 2. Let json be the Unicode string that results from interpreting data according to UTF-8.
    // 3. Convert json to UTF-16.
    auto json = MUST(String::from_utf8(data));

    // 4. Let result be the object literal that results from executing the JSON.parse internal function
    //    in the context of a new global object, with text argument set to a JavaScript String containing json.
    auto maybe_json_value = JsonValue::from_string(json);
    if (maybe_json_value.is_error())
        return vm.throw_completion<WebIDL::SyntaxError>(JS::ErrorType::JsonMalformed);

    auto json_value = maybe_json_value.release_value();
    if (!json_value.is_object()) {
        return vm.throw_completion<WebIDL::SyntaxError>("JSON value is not an object"_string);
    }

    auto const& json_object = json_value.as_object();

    // 5. Let key be the result of converting result to the IDL dictionary type of JsonWebKey.
    JsonWebKey key {};
    JWK_PARSE_STRING_PROPERTY(kty);
    JWK_PARSE_STRING_PROPERTY(use);
    JWK_PARSE_STRING_PROPERTY(alg);
    JWK_PARSE_STRING_PROPERTY(crv);
    JWK_PARSE_STRING_PROPERTY(x);
    JWK_PARSE_STRING_PROPERTY(y);
    JWK_PARSE_STRING_PROPERTY(d);
    JWK_PARSE_STRING_PROPERTY(n);
    JWK_PARSE_STRING_PROPERTY(e);
    JWK_PARSE_STRING_PROPERTY(p);
    JWK_PARSE_STRING_PROPERTY(q);
    JWK_PARSE_STRING_PROPERTY(dp);
    JWK_PARSE_STRING_PROPERTY(dq);
    JWK_PARSE_STRING_PROPERTY(qi);
    JWK_PARSE_STRING_PROPERTY(k);

    key.ext = json_object.get_bool("ext"sv);

    if (auto key_ops = json_object.get_array("key_ops"sv); key_ops.has_value()) {
        key.key_ops = Vector<String> {};
        key.key_ops->ensure_capacity(key_ops->size());

        key_ops->for_each([&](auto const& value) {
            key.key_ops->append(MUST(String::from_byte_string(value.as_string())));
        });
    }

    if (json_object.has("oth"sv))
        TODO();

    // 6. If the kty field of key is not defined, then throw a DataError.
    if (!key.kty.has_value())
        return vm.throw_completion<WebIDL::DataError>("kty field is not defined"_string);

    // 7. Return key.
    return key;
}

#undef JWK_PARSE_STRING_PROPERTY

JS::ThrowCompletionOr<GC::Ref<JS::Object>> JsonWebKey::to_object(JS::Realm& realm)
{
    auto& vm = realm.vm();
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());

    if (kty.has_value())
        TRY(object->create_data_property("kty", JS::PrimitiveString::create(vm, kty.value())));

    if (use.has_value())
        TRY(object->create_data_property("use", JS::PrimitiveString::create(vm, use.value())));

    if (key_ops.has_value()) {
        auto key_ops_array = JS::Array::create_from<String>(realm, key_ops.value().span(), [&](auto& key_usage) -> JS::Value {
            return JS::PrimitiveString::create(realm.vm(), key_usage);
        });
        TRY(object->create_data_property("key_ops", move(key_ops_array)));
    }

    if (alg.has_value())
        TRY(object->create_data_property("alg", JS::PrimitiveString::create(vm, alg.value())));

    if (ext.has_value())
        TRY(object->create_data_property("ext", JS::Value(ext.value())));

    if (crv.has_value())
        TRY(object->create_data_property("crv", JS::PrimitiveString::create(vm, crv.value())));

    if (x.has_value())
        TRY(object->create_data_property("x", JS::PrimitiveString::create(vm, x.value())));

    if (y.has_value())
        TRY(object->create_data_property("y", JS::PrimitiveString::create(vm, y.value())));

    if (d.has_value())
        TRY(object->create_data_property("d", JS::PrimitiveString::create(vm, d.value())));

    if (n.has_value())
        TRY(object->create_data_property("n", JS::PrimitiveString::create(vm, n.value())));

    if (e.has_value())
        TRY(object->create_data_property("e", JS::PrimitiveString::create(vm, e.value())));

    if (p.has_value())
        TRY(object->create_data_property("p", JS::PrimitiveString::create(vm, p.value())));

    if (q.has_value())
        TRY(object->create_data_property("q", JS::PrimitiveString::create(vm, q.value())));

    if (dp.has_value())
        TRY(object->create_data_property("dp", JS::PrimitiveString::create(vm, dp.value())));

    if (dq.has_value())
        TRY(object->create_data_property("dq", JS::PrimitiveString::create(vm, dq.value())));

    if (qi.has_value())
        TRY(object->create_data_property("qi", JS::PrimitiveString::create(vm, qi.value())));

    if (oth.has_value()) {
        TODO();
    }

    if (k.has_value())
        TRY(object->create_data_property("k", JS::PrimitiveString::create(vm, k.value())));

    return object;
}

}
