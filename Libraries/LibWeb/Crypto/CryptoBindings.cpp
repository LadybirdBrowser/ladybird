/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/CryptoKey.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Crypto/CryptoAlgorithms.h>
#include <LibWeb/Crypto/CryptoBindings.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Crypto {

#define JWK_PARSE_STRING_PROPERTY(name)                                      \
    if (auto value = json_object.get_string(#name##sv); value.has_value()) { \
        key.name = value.release_value();                                    \
    }

JS::ThrowCompletionOr<JsonWebKey> parse_json_web_key(JS::Realm& realm, ReadonlyBytes data)
{
    // 1. Let data be the sequence of bytes to be parsed.

    // 2. Let json be the Unicode string that results from interpreting data according to UTF-8.
    // 3. Convert json to UTF-16.
    auto json = MUST(String::from_utf8(data));

    // 4. Let result be the object literal that results from executing the JSON.parse internal function
    //    in the context of a new global object, with text argument set to a JavaScript String containing json.
    auto maybe_json_value = JsonValue::from_string(json);
    if (maybe_json_value.is_error())
        return throw_completion(realm, WebIDL::SyntaxError::create(JS::ErrorType::JsonMalformed.message()));

    auto json_value = maybe_json_value.release_value();
    if (!json_value.is_object()) {
        return throw_completion(realm, WebIDL::SyntaxError::create("JSON value is not an object"_utf16));
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
    JWK_PARSE_STRING_PROPERTY(pub);
    JWK_PARSE_STRING_PROPERTY(priv);

    key.ext = json_object.get_bool("ext"sv);

    if (auto key_ops = json_object.get_array("key_ops"sv); key_ops.has_value()) {
        key.key_ops = Vector<String> {};
        key.key_ops->ensure_capacity(key_ops->size());

        key_ops->for_each([&](auto const& value) {
            key.key_ops->append(value.as_string());
        });
    }

    if (json_object.has("oth"sv))
        TODO();

    // 6. If the kty field of key is not defined, then throw a DataError.
    if (!key.kty.has_value())
        return throw_completion(realm, WebIDL::DataError::create("kty field is not defined"_utf16));

    // 7. Return key.
    return key;
}

#undef JWK_PARSE_STRING_PROPERTY

void resolve_crypto_key_promise(JS::Realm& realm, WebIDL::Promise& promise, GC::Ref<CryptoKey> key)
{
    WebIDL::resolve_promise(realm, promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, key));
}

JS::ThrowCompletionOr<GC::Ref<JS::Object>> encapsulated_bits(JS::Realm& realm, EncapsulatedBits const& encapsulated_bits)
{
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());

    if (encapsulated_bits.shared_key.has_value())
        TRY(object->create_data_property("sharedKey"_utf16_fly_string, JS::ArrayBuffer::create(realm, encapsulated_bits.shared_key.value())));

    if (encapsulated_bits.ciphertext.has_value())
        TRY(object->create_data_property("ciphertext"_utf16_fly_string, JS::ArrayBuffer::create(realm, encapsulated_bits.ciphertext.value())));

    return object;
}

JS::ThrowCompletionOr<GC::Ref<JS::Object>> encapsulated_key(JS::Realm& realm, EncapsulatedKey const& encapsulated_key)
{
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());

    if (encapsulated_key.shared_key.has_value())
        TRY(object->create_data_property("sharedKey"_utf16_fly_string, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { *encapsulated_key.shared_key.value() })));

    if (encapsulated_key.ciphertext.has_value())
        TRY(object->create_data_property("ciphertext"_utf16_fly_string, JS::ArrayBuffer::create(realm, encapsulated_key.ciphertext.value())));

    return object;
}

JS::Value crypto_key(JS::Realm& realm, GC::Ref<CryptoKey> key)
{
    return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, key);
}

JS::ThrowCompletionOr<GC::Ref<JS::Object>> crypto_key_pair(JS::Realm& realm, CryptoKeyPair const& key_pair)
{
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    TRY(object->create_data_property_or_throw("publicKey"_utf16_fly_string, crypto_key(realm, key_pair.public_key)));
    TRY(object->create_data_property_or_throw("privateKey"_utf16_fly_string, crypto_key(realm, key_pair.private_key)));
    return object;
}

JS::ThrowCompletionOr<GC::Ref<CryptoKey>> crypto_key_from_value(JS::VM& vm, JS::Value value)
{
    auto key_object = TRY(value.to_object(vm));

    auto* key = Bindings::impl_from<CryptoKey>(&*key_object);
    if (!key)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "CryptoKey");

    return GC::Ref { *key };
}

}
