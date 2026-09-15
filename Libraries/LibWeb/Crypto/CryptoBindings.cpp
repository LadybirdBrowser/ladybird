/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

// https://w3c.github.io/webcrypto/#concept-parse-a-jwk
WebIDL::ExceptionOr<JsonWebKey> parse_json_web_key(JS::Realm& realm, ReadonlyBytes data)
{
    // 1. Let data be the sequence of bytes to be parsed.

    // 2. Let json be the Unicode string that results from interpreting data according to UTF-8.
    // 3. Convert json to UTF-16.
    auto json = Utf16String::from_utf8_with_replacement_character(StringView { data }, Utf16String::WithBOMHandling::Yes);

    // 4. Let result be the object literal that results from executing the JSON.parse internal function
    //    in the context of a new global object, with text argument set to a JavaScript String containing json.
    // 5. Let key be the result of converting result to the IDL dictionary type of JsonWebKey.
    auto key = TRY(Bindings::convert_json_to_idl_value_for_json_web_key(realm.vm(), json));

    // 6. If the kty field of key is not defined, then throw a DataError.
    if (!key.kty.has_value())
        return WebIDL::DataError::create("kty field is not defined"_utf16);

    // 7. Return key.
    return key;
}

void resolve_crypto_key_promise(JS::Realm& realm, WebIDL::Promise& promise, GC::Ref<CryptoKey> key)
{
    WebIDL::resolve_promise(promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, key));
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
