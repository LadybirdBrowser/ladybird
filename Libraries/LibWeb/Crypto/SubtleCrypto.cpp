/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/QuickSort.h>
#include <LibCrypto/Hash/HashManager.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SubtleCryptoPrototype.h>
#include <LibWeb/Crypto/KeyAlgorithms.h>
#include <LibWeb/Crypto/SubtleCrypto.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Crypto {

static void normalize_key_usages(Vector<Bindings::KeyUsage>& key_usages)
{
    quick_sort(key_usages);
}
struct RegisteredAlgorithm {
    NonnullOwnPtr<AlgorithmMethods> (*create_methods)(JS::Realm&) = nullptr;
    JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> (*parameter_from_value)(JS::VM&, JS::Value) = nullptr;
};
using SupportedAlgorithmsMap = HashMap<String, HashMap<String, RegisteredAlgorithm, AK::ASCIICaseInsensitiveStringTraits>>;

static SupportedAlgorithmsMap& supported_algorithms_internal();
static SupportedAlgorithmsMap const& supported_algorithms();

template<typename Methods, typename Param = AlgorithmParams>
static void define_an_algorithm(String op, String algorithm);

GC_DEFINE_ALLOCATOR(SubtleCrypto);

GC::Ref<SubtleCrypto> SubtleCrypto::create(JS::Realm& realm)
{
    return realm.create<SubtleCrypto>(realm);
}

SubtleCrypto::SubtleCrypto(JS::Realm& realm)
    : PlatformObject(realm)
{
}

SubtleCrypto::~SubtleCrypto() = default;

void SubtleCrypto::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SubtleCrypto);
}

// https://w3c.github.io/webcrypto/#dfn-normalize-an-algorithm
WebIDL::ExceptionOr<NormalizedAlgorithmAndParameter> normalize_an_algorithm(JS::Realm& realm, AlgorithmIdentifier const& algorithm, String operation)
{
    auto& vm = realm.vm();

    // If alg is an instance of a DOMString:
    if (algorithm.has<String>()) {
        // Return the result of running the normalize an algorithm algorithm,
        // with the alg set to a new Algorithm dictionary whose name attribute is alg, and with the op set to op.
        auto dictionary = GC::make_root(JS::Object::create(realm, realm.intrinsics().object_prototype()));
        TRY(dictionary->create_data_property("name"_fly_string, JS::PrimitiveString::create(vm, algorithm.get<String>())));

        return normalize_an_algorithm(realm, dictionary, operation);
    }

    // If alg is an object:
    // 1. Let registeredAlgorithms be the associative container stored at the op key of supportedAlgorithms.
    // NOTE: There should always be a container at the op key.
    auto const& internal_object = supported_algorithms();
    auto maybe_registered_algorithms = internal_object.get(operation);
    auto registered_algorithms = maybe_registered_algorithms.value();

    // 2. Let initialAlg be the result of converting the ECMAScript object represented by alg to
    // the IDL dictionary type Algorithm, as defined by [WebIDL].
    // 3. If an error occurred, return the error and terminate this algorithm.
    // Note: We're not going to bother creating an Algorithm object, all we want is the name attribute so that we can
    //       fetch the actual algorithm factory from the registeredAlgorithms map.
    auto initial_algorithm = TRY(algorithm.get<GC::Root<JS::Object>>()->get("name"_fly_string));

    if (initial_algorithm.is_undefined()) {
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Algorithm");
    }

    // 4. Let algName be the value of the name attribute of initialAlg.
    auto algorithm_name = TRY(initial_algorithm.to_string(vm));

    RegisteredAlgorithm desired_type;

    // 5. If registeredAlgorithms contains a key that is a case-insensitive string match for algName:
    if (auto it = registered_algorithms.find(algorithm_name); it != registered_algorithms.end()) {
        // 1. Set algName to the value of the matching key.
        algorithm_name = it->key;

        // 2. Let desiredType be the IDL dictionary type stored at algName in registeredAlgorithms.
        desired_type = it->value;
    } else {
        // Otherwise:
        // Return a new NotSupportedError and terminate this algorithm.
        return WebIDL::NotSupportedError::create(realm, MUST(String::formatted("Algorithm '{}' is not supported for operation '{}'", algorithm_name, operation)));
    }

    // 8. Let normalizedAlgorithm be the result of converting the ECMAScript object represented by alg
    // to the IDL dictionary type desiredType, as defined by [WebIDL].
    // 10. If an error occurred, return the error and terminate this algorithm.
    // 11. Let dictionaries be a list consisting of the IDL dictionary type desiredType
    // and all of desiredType's inherited dictionaries, in order from least to most derived.
    // 12. For each dictionary dictionary in dictionaries:
    //    Note: All of these steps are handled by the create_methods and parameter_from_value methods.
    auto methods = desired_type.create_methods(realm);
    auto parameter = TRY(desired_type.parameter_from_value(vm, algorithm.get<GC::Root<JS::Object>>()));

    // 9. Set the name attribute of normalizedAlgorithm to algName.
    VERIFY(parameter->name.is_empty());
    parameter->name = algorithm_name;

    auto normalized_algorithm = NormalizedAlgorithmAndParameter { move(methods), move(parameter) };

    // 13. Return normalizedAlgorithm.
    return normalized_algorithm;
}

// https://w3c.github.io/webcrypto/#dfn-SubtleCrypto-method-encrypt
GC::Ref<WebIDL::Promise> SubtleCrypto::encrypt(AlgorithmIdentifier const& algorithm, GC::Ref<CryptoKey> key, GC::Root<WebIDL::BufferSource> const& data_parameter)
{
    auto& realm = this->realm();
    auto& vm = this->vm();
    // 1. Let algorithm and key be the algorithm and key parameters passed to the encrypt() method, respectively.

    // 2. Let data be the result of getting a copy of the bytes held by the data parameter passed to the encrypt() method.
    auto data_or_error = WebIDL::get_buffer_source_copy(*data_parameter->raw_object());
    if (data_or_error.is_error()) {
        VERIFY(data_or_error.error().code() == ENOMEM);
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory)));
    }
    auto data = data_or_error.release_value();

    // 3. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "encrypt".
    auto normalized_algorithm = normalize_an_algorithm(realm, algorithm, "encrypt"_string);

    // 4. If an error occurred, return a Promise rejected with normalizedAlgorithm.
    if (normalized_algorithm.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_algorithm.release_error());

    // 5. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 6. Return promise and perform the remaining steps in parallel.

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, normalized_algorithm = normalized_algorithm.release_value(), promise, key, data = move(data)]() -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 7. If the following steps or referenced procedures say to throw an error, reject promise with the returned error and then terminate the algorithm.

        // 8. If the name member of normalizedAlgorithm is not equal to the name attribute of the [[algorithm]] internal slot of key then throw an InvalidAccessError.
        if (normalized_algorithm.parameter->name != key->algorithm_name()) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Algorithm mismatch"_string));
            return;
        }

        // 9. If the [[usages]] internal slot of key does not contain an entry that is "encrypt", then throw an InvalidAccessError.
        if (!key->internal_usages().contains_slow(Bindings::KeyUsage::Encrypt)) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Key does not support encryption"_string));
            return;
        }

        // 10. Let ciphertext be the result of performing the encrypt operation specified by normalizedAlgorithm using algorithm and key and with data as plaintext.
        auto cipher_text = normalized_algorithm.methods->encrypt(*normalized_algorithm.parameter, key, data);
        if (cipher_text.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), cipher_text.release_error()).release_value().value());
            return;
        }

        // 9. Resolve promise with ciphertext.
        WebIDL::resolve_promise(realm, promise, cipher_text.release_value());
    }));

    return promise;
}

// https://w3c.github.io/webcrypto/#dfn-SubtleCrypto-method-decrypt
GC::Ref<WebIDL::Promise> SubtleCrypto::decrypt(AlgorithmIdentifier const& algorithm, GC::Ref<CryptoKey> key, GC::Root<WebIDL::BufferSource> const& data_parameter)
{
    auto& realm = this->realm();
    auto& vm = this->vm();
    // 1. Let algorithm and key be the algorithm and key parameters passed to the decrypt() method, respectively.

    // 2. Let data be the result of getting a copy of the bytes held by the data parameter passed to the decrypt() method.
    auto data_or_error = WebIDL::get_buffer_source_copy(*data_parameter->raw_object());
    if (data_or_error.is_error()) {
        VERIFY(data_or_error.error().code() == ENOMEM);
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory)));
    }
    auto data = data_or_error.release_value();

    // 3. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "decrypt".
    auto normalized_algorithm = normalize_an_algorithm(realm, algorithm, "decrypt"_string);

    // 4. If an error occurred, return a Promise rejected with normalizedAlgorithm.
    if (normalized_algorithm.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_algorithm.release_error());

    // 5. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 6. Return promise and perform the remaining steps in parallel.

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, normalized_algorithm = normalized_algorithm.release_value(), promise, key, data = move(data)]() -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 7. If the following steps or referenced procedures say to throw an error, reject promise with the returned error and then terminate the algorithm.

        // 8. If the name member of normalizedAlgorithm is not equal to the name attribute of the [[algorithm]] internal slot of key then throw an InvalidAccessError.
        if (normalized_algorithm.parameter->name != key->algorithm_name()) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Algorithm mismatch"_string));
            return;
        }

        // 9. If the [[usages]] internal slot of key does not contain an entry that is "decrypt", then throw an InvalidAccessError.
        if (!key->internal_usages().contains_slow(Bindings::KeyUsage::Decrypt)) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Key does not support encryption"_string));
            return;
        }

        // 10. Let plaintext be the result of performing the decrypt operation specified by normalizedAlgorithm using algorithm and key and with data as ciphertext.
        auto plain_text = normalized_algorithm.methods->decrypt(*normalized_algorithm.parameter, key, data);
        if (plain_text.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), plain_text.release_error()).release_value().value());
            return;
        }

        // 9. Resolve promise with plaintext.
        WebIDL::resolve_promise(realm, promise, plain_text.release_value());
    }));

    return promise;
}

// https://w3c.github.io/webcrypto/#dfn-SubtleCrypto-method-digest
GC::Ref<WebIDL::Promise> SubtleCrypto::digest(AlgorithmIdentifier const& algorithm, GC::Root<WebIDL::BufferSource> const& data)
{
    auto& realm = this->realm();
    auto& vm = this->vm();

    // 1. Let algorithm be the algorithm parameter passed to the digest() method.

    // 2. Let data be the result of getting a copy of the bytes held by the data parameter passed to the digest() method.
    auto data_buffer_or_error = WebIDL::get_buffer_source_copy(*data->raw_object());
    if (data_buffer_or_error.is_error()) {
        VERIFY(data_buffer_or_error.error().code() == ENOMEM);
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory)));
    }
    auto data_buffer = data_buffer_or_error.release_value();

    // 3. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "digest".
    auto normalized_algorithm = normalize_an_algorithm(realm, algorithm, "digest"_string);

    // 4. If an error occurred, return a Promise rejected with normalizedAlgorithm.
    // FIXME: Spec bug: link to https://webidl.spec.whatwg.org/#a-promise-rejected-with
    if (normalized_algorithm.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_algorithm.release_error());

    // 5. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 6. Return promise and perform the remaining steps in parallel.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, algorithm_object = normalized_algorithm.release_value(), promise, data_buffer = move(data_buffer)]() -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 7. If the following steps or referenced procedures say to throw an error, reject promise with the returned error and then terminate the algorithm.
        // FIXME: Need spec reference to https://webidl.spec.whatwg.org/#reject

        // 8. Let result be the result of performing the digest operation specified by normalizedAlgorithm using algorithm, with data as message.
        auto result = algorithm_object.methods->digest(*algorithm_object.parameter, data_buffer);

        if (result.is_exception()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), result.release_error()).release_value().value());
            return;
        }

        // 9. Resolve promise with result.
        WebIDL::resolve_promise(realm, promise, result.release_value());
    }));

    return promise;
}

// https://w3c.github.io/webcrypto/#dfn-SubtleCrypto-method-generateKey
GC::Ref<WebIDL::Promise> SubtleCrypto::generate_key(AlgorithmIdentifier algorithm, bool extractable, Vector<Bindings::KeyUsage> key_usages)
{
    auto& realm = this->realm();

    // 1. Let algorithm, extractable and usages be the algorithm, extractable and keyUsages
    //    parameters passed to the generateKey() method, respectively.

    // 2. Let normalizedAlgorithm be the result of normalizing an algorithm,
    //    with alg set to algorithm and op set to "generateKey".
    auto normalized_algorithm = normalize_an_algorithm(realm, algorithm, "generateKey"_string);

    // 3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
    if (normalized_algorithm.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_algorithm.release_error());

    // 4. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 5. Return promise and perform the remaining steps in parallel.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, normalized_algorithm = normalized_algorithm.release_value(), promise, extractable, key_usages = move(key_usages)]() -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 6. If the following steps or referenced procedures say to throw an error, reject promise with
        //    the returned error and then terminate the algorithm.

        // 7. Let result be the result of performing the generate key operation specified by normalizedAlgorithm
        //    using algorithm, extractable and usages.
        auto result_or_error = normalized_algorithm.methods->generate_key(*normalized_algorithm.parameter, extractable, key_usages);

        if (result_or_error.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), result_or_error.release_error()).release_value().value());
            return;
        }
        auto result = result_or_error.release_value();

        // 8. If result is a CryptoKey object:
        //      If the [[type]] internal slot of result is "secret" or "private" and usages is empty, then throw a SyntaxError.
        //    If result is a CryptoKeyPair object:
        //      If the [[usages]] internal slot of the privateKey attribute of result is the empty sequence, then throw a SyntaxError.
        // 9. Resolve promise with result.
        result.visit(
            [&](GC::Ref<CryptoKey>& key) {
                if ((key->type() == Bindings::KeyType::Secret || key->type() == Bindings::KeyType::Private) && key_usages.is_empty()) {
                    WebIDL::reject_promise(realm, promise, WebIDL::SyntaxError::create(realm, "usages must not be empty"_string));
                    return;
                }
                WebIDL::resolve_promise(realm, promise, key);
            },
            [&](GC::Ref<CryptoKeyPair>& key_pair) {
                if (key_pair->private_key()->internal_usages().is_empty()) {
                    WebIDL::reject_promise(realm, promise, WebIDL::SyntaxError::create(realm, "usages must not be empty"_string));
                    return;
                }
                WebIDL::resolve_promise(realm, promise, key_pair);
            });
    }));

    return promise;
}

// https://w3c.github.io/webcrypto/#SubtleCrypto-method-importKey
JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> SubtleCrypto::import_key(Bindings::KeyFormat format, KeyDataType key_data, AlgorithmIdentifier algorithm, bool extractable, Vector<Bindings::KeyUsage> key_usages)
{
    auto& realm = this->realm();

    // 1. Let format, algorithm, extractable and usages, be the format, algorithm, extractable
    // and key_usages parameters passed to the importKey() method, respectively.

    Variant<ByteBuffer, Bindings::JsonWebKey, Empty> real_key_data;
    // 2. If format is equal to the string "raw", "pkcs8", or "spki":
    if (format == Bindings::KeyFormat::Raw || format == Bindings::KeyFormat::Pkcs8 || format == Bindings::KeyFormat::Spki) {
        // 1. If the keyData parameter passed to the importKey() method is a JsonWebKey dictionary, throw a TypeError.
        if (key_data.has<Bindings::JsonWebKey>()) {
            return realm.vm().throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "BufferSource");
        }

        // 2. Let keyData be the result of getting a copy of the bytes held by the keyData parameter passed to the importKey() method.
        real_key_data = MUST(WebIDL::get_buffer_source_copy(*key_data.get<GC::Root<WebIDL::BufferSource>>()->raw_object()));
    }

    if (format == Bindings::KeyFormat::Jwk) {
        // 1. If the keyData parameter passed to the importKey() method is not a JsonWebKey dictionary, throw a TypeError.
        if (!key_data.has<Bindings::JsonWebKey>()) {
            return realm.vm().throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "JsonWebKey");
        }

        // 2. Let keyData be the keyData parameter passed to the importKey() method.
        real_key_data = key_data.get<Bindings::JsonWebKey>();
    }

    // NOTE: The spec jumps to 5 here for some reason?
    // 5. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "importKey".
    auto normalized_algorithm = normalize_an_algorithm(realm, algorithm, "importKey"_string);

    // 6. If an error occurred, return a Promise rejected with normalizedAlgorithm.
    if (normalized_algorithm.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_algorithm.release_error());

    // 7. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 8. Return promise and perform the remaining steps in parallel.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [&realm, real_key_data = move(real_key_data), normalized_algorithm = normalized_algorithm.release_value(), promise, format, extractable, key_usages = move(key_usages), algorithm = move(algorithm)]() mutable -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 9. If the following steps or referenced procedures say to throw an error, reject promise with the returned error and then terminate the algorithm.

        // 10. Let result be the CryptoKey object that results from performing the import key operation
        // specified by normalizedAlgorithm using keyData, algorithm, format, extractable and usages.
        auto maybe_result = normalized_algorithm.methods->import_key(*normalized_algorithm.parameter, format, real_key_data.downcast<CryptoKey::InternalKeyData>(), extractable, key_usages);
        if (maybe_result.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), maybe_result.release_error()).release_value().value());
            return;
        }
        auto result = maybe_result.release_value();

        // 11. If the [[type]] internal slot of result is "secret" or "private" and usages is empty, then throw a SyntaxError.
        if ((result->type() == Bindings::KeyType::Secret || result->type() == Bindings::KeyType::Private) && key_usages.is_empty()) {
            WebIDL::reject_promise(realm, promise, WebIDL::SyntaxError::create(realm, "usages must not be empty"_string));
            return;
        }

        // 12. Set the [[extractable]] internal slot of result to extractable.
        result->set_extractable(extractable);

        // 13. Set the [[usages]] internal slot of result to the normalized value of usages.
        normalize_key_usages(key_usages);
        result->set_usages(key_usages);

        // 14. Resolve promise with result.
        WebIDL::resolve_promise(realm, promise, result);
    }));

    return promise;
}

// https://w3c.github.io/webcrypto/#dfn-SubtleCrypto-method-exportKey
GC::Ref<WebIDL::Promise> SubtleCrypto::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    auto& realm = this->realm();
    // 1. Let format and key be the format and key parameters passed to the exportKey() method, respectively.

    // 2. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 3. Return promise and perform the remaining steps in parallel.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [&realm, key, promise, format]() -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 4.  If the following steps or referenced procedures say to throw an error, reject promise with the returned error and then terminate the algorithm.

        // 5. If the name member of the [[algorithm]] internal slot of key does not identify a registered algorithm that supports the export key operation,
        //    then throw a NotSupportedError.
        // Note: Handled by the base AlgorithmMethods implementation
        auto& algorithm = as<KeyAlgorithm>(*key->algorithm());
        // FIXME: Stash the AlgorithmMethods on the KeyAlgorithm
        auto normalized_algorithm_or_error = normalize_an_algorithm(realm, algorithm.name(), "exportKey"_string);
        if (normalized_algorithm_or_error.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), normalized_algorithm_or_error.release_error()).release_value().value());
            return;
        }
        auto normalized_algorithm = normalized_algorithm_or_error.release_value();

        // 6. If the [[extractable]] internal slot of key is false, then throw an InvalidAccessError.
        if (!key->extractable()) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Key is not extractable"_string));
            return;
        }

        // 7. Let result be the result of performing the export key operation specified by the [[algorithm]] internal slot of key using key and format.
        auto result_or_error = normalized_algorithm.methods->export_key(format, key);
        if (result_or_error.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), result_or_error.release_error()).release_value().value());
            return;
        }

        // 8. Resolve promise with result.
        WebIDL::resolve_promise(realm, promise, result_or_error.release_value());
    }));

    return promise;
}

// https://w3c.github.io/webcrypto/#dfn-SubtleCrypto-method-sign
GC::Ref<WebIDL::Promise> SubtleCrypto::sign(AlgorithmIdentifier const& algorithm, GC::Ref<CryptoKey> key, GC::Root<WebIDL::BufferSource> const& data_parameter)
{
    auto& realm = this->realm();
    auto& vm = this->vm();
    // 1. Let algorithm and key be the algorithm and key parameters passed to the sign() method, respectively.

    // 2. Let data be the result of getting a copy of the bytes held by the data parameter passed to the sign() method.
    auto data_or_error = WebIDL::get_buffer_source_copy(*data_parameter->raw_object());
    if (data_or_error.is_error()) {
        VERIFY(data_or_error.error().code() == ENOMEM);
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory)));
    }
    auto data = data_or_error.release_value();

    // 3. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "sign".
    auto normalized_algorithm = normalize_an_algorithm(realm, algorithm, "sign"_string);

    // 4. If an error occurred, return a Promise rejected with normalizedAlgorithm.
    if (normalized_algorithm.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_algorithm.release_error());

    // 5. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 6. Return promise and perform the remaining steps in parallel.

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, normalized_algorithm = normalized_algorithm.release_value(), promise, key, data = move(data)]() -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 7. If the following steps or referenced procedures say to throw an error, reject promise with the returned error and then terminate the algorithm.

        // 8. If the name member of normalizedAlgorithm is not equal to the name attribute of the [[algorithm]] internal slot of key then throw an InvalidAccessError.
        if (normalized_algorithm.parameter->name != key->algorithm_name()) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Algorithm mismatch"_string));
            return;
        }

        // 9. If the [[usages]] internal slot of key does not contain an entry that is "sign", then throw an InvalidAccessError.
        if (!key->internal_usages().contains_slow(Bindings::KeyUsage::Sign)) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Key does not support signing"_string));
            return;
        }

        // 10. Let result be the result of performing the sign operation specified by normalizedAlgorithm using key and algorithm and with data as message.
        auto result = normalized_algorithm.methods->sign(*normalized_algorithm.parameter, key, data);
        if (result.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), result.release_error()).release_value().value());
            return;
        }

        // 9. Resolve promise with result.
        WebIDL::resolve_promise(realm, promise, result.release_value());
    }));

    return promise;
}

// https://w3c.github.io/webcrypto/#dfn-SubtleCrypto-method-verify
GC::Ref<WebIDL::Promise> SubtleCrypto::verify(AlgorithmIdentifier const& algorithm, GC::Ref<CryptoKey> key, GC::Root<WebIDL::BufferSource> const& signature_data, GC::Root<WebIDL::BufferSource> const& data_parameter)
{
    auto& realm = this->realm();
    auto& vm = this->vm();
    // 1. Let algorithm and key be the algorithm and key parameters passed to the verify() method, respectively.

    // 2. Let signature be the result of getting a copy of the bytes held by the signature parameter passed to the verify() method.
    auto signature_or_error = WebIDL::get_buffer_source_copy(*signature_data->raw_object());
    if (signature_or_error.is_error()) {
        VERIFY(signature_or_error.error().code() == ENOMEM);
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory)));
    }
    auto signature = signature_or_error.release_value();

    // 3. Let data be the result of getting a copy of the bytes held by the data parameter passed to the verify() method.
    auto data_or_error = WebIDL::get_buffer_source_copy(*data_parameter->raw_object());
    if (data_or_error.is_error()) {
        VERIFY(data_or_error.error().code() == ENOMEM);
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::InternalError>(vm.error_message(JS::VM::ErrorMessage::OutOfMemory)));
    }
    auto data = data_or_error.release_value();

    // 3. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "verify".
    auto normalized_algorithm = normalize_an_algorithm(realm, algorithm, "verify"_string);

    // 5. If an error occurred, return a Promise rejected with normalizedAlgorithm.
    if (normalized_algorithm.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_algorithm.release_error());

    // 6. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 7. Return promise and perform the remaining steps in parallel.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, normalized_algorithm = normalized_algorithm.release_value(), promise, key, signature = move(signature), data = move(data)]() -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 8. If the following steps or referenced procedures say to throw an error, reject promise with the returned error and then terminate the algorithm.

        // 9. If the name member of normalizedAlgorithm is not equal to the name attribute of the [[algorithm]] internal slot of key then throw an InvalidAccessError.
        if (normalized_algorithm.parameter->name != key->algorithm_name()) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Algorithm mismatch"_string));
            return;
        }

        // 10. If the [[usages]] internal slot of key does not contain an entry that is "verify", then throw an InvalidAccessError.
        if (!key->internal_usages().contains_slow(Bindings::KeyUsage::Verify)) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Key does not support verification"_string));
            return;
        }

        // 11. Let result be the result of performing the verify operation specified by normalizedAlgorithm using key, algorithm and signature and with data as message.
        auto result = normalized_algorithm.methods->verify(*normalized_algorithm.parameter, key, signature, data);
        if (result.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), result.release_error()).release_value().value());
            return;
        }

        // 12. Resolve promise with result.
        WebIDL::resolve_promise(realm, promise, result.release_value());
    }));

    return promise;
}

// https://w3c.github.io/webcrypto/#SubtleCrypto-method-deriveBits
GC::Ref<WebIDL::Promise> SubtleCrypto::derive_bits(AlgorithmIdentifier algorithm, GC::Ref<CryptoKey> base_key, Optional<u32> length_optional)
{
    auto& realm = this->realm();
    // 1. Let algorithm, baseKey and length, be the algorithm, baseKey and length parameters passed to the deriveBits() method, respectively.

    // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "deriveBits".
    auto normalized_algorithm = normalize_an_algorithm(realm, algorithm, "deriveBits"_string);

    // 3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
    if (normalized_algorithm.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_algorithm.release_error());

    // 4. Let promise be a new Promise object.
    auto promise = WebIDL::create_promise(realm);

    // 5. Return promise and perform the remaining steps in parallel.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, normalized_algorithm = normalized_algorithm.release_value(), promise, base_key, length_optional]() -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 6. If the following steps or referenced procedures say to throw an error, reject promise with the returned error and then terminate the algorithm.

        // 7. If the name member of normalizedAlgorithm is not equal to the name attribute of the [[algorithm]] internal slot of baseKey then throw an InvalidAccessError.
        if (normalized_algorithm.parameter->name != base_key->algorithm_name()) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Algorithm mismatch"_string));
            return;
        }

        // 8. If the [[usages]] internal slot of baseKey does not contain an entry that is "deriveBits", then throw an InvalidAccessError.
        if (!base_key->internal_usages().contains_slow(Bindings::KeyUsage::Derivebits)) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Key does not support deriving bits"_string));
            return;
        }

        // 9. Let result be the result of creating an ArrayBuffer containing the result of performing the derive bits operation specified by normalizedAlgorithm using baseKey, algorithm and length.
        auto result = normalized_algorithm.methods->derive_bits(*normalized_algorithm.parameter, base_key, length_optional);
        if (result.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), result.release_error()).release_value().value());
            return;
        }

        // 10. Resolve promise with result.
        WebIDL::resolve_promise(realm, promise, result.release_value());
    }));

    return promise;
}

// https://w3c.github.io/webcrypto/#SubtleCrypto-method-deriveKey
GC::Ref<WebIDL::Promise> SubtleCrypto::derive_key(AlgorithmIdentifier algorithm, GC::Ref<CryptoKey> base_key, AlgorithmIdentifier derived_key_type, bool extractable, Vector<Bindings::KeyUsage> key_usages)
{
    auto& realm = this->realm();
    auto& vm = this->vm();
    // 1. Let algorithm, baseKey, derivedKeyType, extractable and usages be the algorithm, baseKey, derivedKeyType, extractable and keyUsages parameters passed to the deriveKey() method, respectively.

    // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "deriveBits".
    auto normalized_algorithm = normalize_an_algorithm(realm, algorithm, "deriveBits"_string);

    // 3. If an error occurred, return a Promise rejected with normalizedAlgorithm.
    if (normalized_algorithm.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_algorithm.release_error());

    // 4. Let normalizedDerivedKeyAlgorithmImport be the result of normalizing an algorithm, with alg set to derivedKeyType and op set to "importKey".
    auto normalized_derived_key_algorithm_import = normalize_an_algorithm(realm, derived_key_type, "importKey"_string);

    // 5. If an error occurred, return a Promise rejected with normalizedDerivedKeyAlgorithmImport.
    if (normalized_derived_key_algorithm_import.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_derived_key_algorithm_import.release_error());

    // 6. Let normalizedDerivedKeyAlgorithmLength be the result of normalizing an algorithm, with alg set to derivedKeyType and op set to "get key length".
    auto normalized_derived_key_algorithm_length = normalize_an_algorithm(realm, derived_key_type, "get key length"_string);

    // 7. If an error occurred, return a Promise rejected with normalizedDerivedKeyAlgorithmLength.
    if (normalized_derived_key_algorithm_length.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_derived_key_algorithm_length.release_error());

    // 8. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 9. Return promise and perform the remaining steps in parallel.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, &vm, normalized_algorithm = normalized_algorithm.release_value(), promise, normalized_derived_key_algorithm_import = normalized_derived_key_algorithm_import.release_value(), normalized_derived_key_algorithm_length = normalized_derived_key_algorithm_length.release_value(), base_key = move(base_key), extractable, key_usages = move(key_usages)]() mutable -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 10. If the following steps or referenced procedures say to throw an error, reject promise with the returned error and then terminate the algorithm.

        // 11. If the name member of normalizedAlgorithm is not equal to the name attribute of the [[algorithm]] internal slot of baseKey then throw an InvalidAccessError.
        if (normalized_algorithm.parameter->name != base_key->algorithm_name()) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Algorithm mismatch"_string));
            return;
        }

        // 12. If the [[usages]] internal slot of baseKey does not contain an entry that is "deriveKey", then throw an InvalidAccessError.
        if (!base_key->internal_usages().contains_slow(Bindings::KeyUsage::Derivekey)) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Key does not support deriving keys"_string));
            return;
        }

        // 13. Let length be the result of performing the get key length algorithm specified by normalizedDerivedKeyAlgorithmLength using derivedKeyType.
        auto length_result = normalized_derived_key_algorithm_length.methods->get_key_length(*normalized_derived_key_algorithm_length.parameter);
        if (length_result.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), length_result.release_error()).release_value().value());
            return;
        }

        auto length_raw_value = length_result.release_value();
        Optional<u32> length = {};
        if (length_raw_value.is_number()) {
            auto maybe_length = length_raw_value.to_u32(vm);
            if (!maybe_length.has_value()) {
                WebIDL::reject_promise(realm, promise, maybe_length.release_error().release_value().value());
                return;
            }

            length = maybe_length.value();
        }

        // 14. Let secret be the result of performing the derive bits operation specified by normalizedAlgorithm using key, algorithm and length.
        auto secret = normalized_algorithm.methods->derive_bits(*normalized_algorithm.parameter, base_key, length);
        if (secret.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), secret.release_error()).release_value().value());
            return;
        }

        // 15. Let result be the result of performing the import key operation specified by normalizedDerivedKeyAlgorithmImport using "raw" as format, secret as keyData, derivedKeyType as algorithm and using extractable and usages.
        auto result_or_error = normalized_derived_key_algorithm_import.methods->import_key(*normalized_derived_key_algorithm_import.parameter, Bindings::KeyFormat::Raw, secret.release_value()->buffer(), extractable, key_usages);
        if (result_or_error.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), result_or_error.release_error()).release_value().value());
            return;
        }
        auto result = result_or_error.release_value();

        // 16. If the [[type]] internal slot of result is "secret" or "private" and usages is empty, then throw a SyntaxError.
        if ((result->type() == Bindings::KeyType::Secret || result->type() == Bindings::KeyType::Private) && key_usages.is_empty()) {
            WebIDL::reject_promise(realm, promise, WebIDL::SyntaxError::create(realm, "usages must not be empty"_string));
            return;
        }

        // 17. Set the [[extractable]] internal slot of result to extractable.
        result->set_extractable(extractable);

        // 18. Set the [[usages]] internal slot of result to the normalized value of usages.
        normalize_key_usages(key_usages);
        result->set_usages(key_usages);

        // 19. Resolve promise with result.
        WebIDL::resolve_promise(realm, promise, result);
    }));

    return promise;
}

// https://w3c.github.io/webcrypto/#SubtleCrypto-method-wrapKey
GC::Ref<WebIDL::Promise> SubtleCrypto::wrap_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key, GC::Ref<CryptoKey> wrapping_key, AlgorithmIdentifier algorithm)
{
    auto& realm = this->realm();
    // 1. Let format, key, wrappingKey and algorithm be the format, key, wrappingKey and wrapAlgorithm parameters passed to the wrapKey() method, respectively.

    StringView operation;
    auto normalized_algorithm_or_error = [&]() -> WebIDL::ExceptionOr<NormalizedAlgorithmAndParameter> {
        // 2. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "wrapKey".
        auto normalized_algorithm_wrap_key_or_error = normalize_an_algorithm(realm, algorithm, "wrapKey"_string);

        // 3. If an error occurred, let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "encrypt".
        // 4. If an error occurred, return a Promise rejected with normalizedAlgorithm.
        if (normalized_algorithm_wrap_key_or_error.is_error()) {
            auto normalized_algorithm_encrypt_or_error = normalize_an_algorithm(realm, algorithm, "encrypt"_string);
            if (normalized_algorithm_encrypt_or_error.is_error())
                return normalized_algorithm_encrypt_or_error.release_error();

            operation = "encrypt"sv;
            return normalized_algorithm_encrypt_or_error.release_value();
        } else {
            operation = "wrapKey"sv;
            return normalized_algorithm_wrap_key_or_error.release_value();
        }
    }();
    if (normalized_algorithm_or_error.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_algorithm_or_error.release_error());

    auto normalized_algorithm = normalized_algorithm_or_error.release_value();

    // 5. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 6. Return promise and perform the remaining steps in parallel.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, normalized_algorithm = move(normalized_algorithm), promise, wrapping_key = move(wrapping_key), key = move(key), format, operation]() mutable -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 7. If the following steps or referenced procedures say to throw an error, reject promise with the returned error and then terminate the algorithm.

        // 8. If the name member of normalizedAlgorithm is not equal to the name attribute
        //    of the [[algorithm]] internal slot of wrappingKey then throw an InvalidAccessError.
        if (normalized_algorithm.parameter->name != wrapping_key->algorithm_name()) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Algorithm mismatch"_string));
            return;
        }

        // 9. If the [[usages]] internal slot of wrappingKey does not contain an entry that is "wrapKey", then throw an InvalidAccessError.
        if (!wrapping_key->internal_usages().contains_slow(Bindings::KeyUsage::Wrapkey)) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Key does not support wrapping keys"_string));
            return;
        }

        // 10. If the algorithm identified by the [[algorithm]] internal slot of key does not support the export key operation, then throw a NotSupportedError.
        // Note: Handled by the base AlgorithmMethods implementation

        // 11. If the [[extractable]] internal slot of key is false, then throw an InvalidAccessError.
        if (!key->extractable()) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Key is not extractable"_string));
            return;
        }

        // 12. Let key be the result of performing the export key operation specified the [[algorithm]] internal slot of key using key and format.
        // NOTE: The spec does not mention we need to normalize this, but it's the only way we have to get to export_key.
        auto& key_algorithm = as<KeyAlgorithm>(*key->algorithm());
        auto normalized_key_algorithm = normalize_an_algorithm(realm, key_algorithm.name(), "exportKey"_string);
        if (normalized_key_algorithm.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), normalized_key_algorithm.release_error()).release_value().value());
            return;
        }

        auto key_data_or_error = normalized_key_algorithm.release_value().methods->export_key(format, key);
        if (key_data_or_error.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), key_data_or_error.release_error()).release_value().value());
            return;
        }

        auto key_data = key_data_or_error.release_value();

        ByteBuffer bytes;
        // 13. If format is equal to the strings "raw", "pkcs8", or "spki":
        if (format == Bindings::KeyFormat::Raw || format == Bindings::KeyFormat::Pkcs8 || format == Bindings::KeyFormat::Spki) {
            // Set bytes be set to key.
            bytes = as<JS::ArrayBuffer>(*key_data).buffer();
        }

        // If format is equal to the string "jwk":
        else if (format == Bindings::KeyFormat::Jwk) {
            // 1. Convert key to an ECMAScript Object, as specified in [WEBIDL],
            //    performing the conversion in the context of a new global object.
            // 2. Let json be the result of representing key as a UTF-16 string conforming to the JSON grammar;
            //    for example, by executing the JSON.stringify algorithm specified in [ECMA-262] in the context of a new global object.
            auto maybe_json = JS::JSONObject::stringify_impl(realm.vm(), key_data, JS::Value {}, JS::Value {});
            if (maybe_json.is_error()) {
                WebIDL::reject_promise(realm, promise, maybe_json.release_error().release_value().value());
                return;
            }

            // 3. Let bytes be the result of UTF-8 encoding json.
            bytes = MUST(ByteBuffer::copy(maybe_json.value()->bytes()));
        } else {
            VERIFY_NOT_REACHED();
        }

        JS::Value result;
        // 14. If normalizedAlgorithm supports the wrap key operation:
        if (operation == "wrapKey") {
            // Let result be the result of performing the wrap key operation specified by normalizedAlgorithm
            // using algorithm, wrappingKey as key and bytes as plaintext.
            auto result_or_error = normalized_algorithm.methods->wrap_key(*normalized_algorithm.parameter, wrapping_key, bytes);
            if (result_or_error.is_error()) {
                WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), result_or_error.release_error()).release_value().value());
                return;
            }

            result = result_or_error.release_value();
        }

        // Otherwise, if normalizedAlgorithm supports the encrypt operation:
        else if (operation == "encrypt") {
            // Let result be the result of performing the encrypt operation specified by normalizedAlgorithm
            // using algorithm, wrappingKey as key and bytes as plaintext.
            auto result_or_error = normalized_algorithm.methods->encrypt(*normalized_algorithm.parameter, wrapping_key, bytes);
            if (result_or_error.is_error()) {
                WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), result_or_error.release_error()).release_value().value());
                return;
            }

            result = result_or_error.release_value();
        }

        // Otherwise:
        else {
            // throw a NotSupportedError.
            WebIDL::reject_promise(realm, promise, WebIDL::NotSupportedError::create(realm, "Algorithm does not support wrapping"_string));
            return;
        }

        // 15. Resolve promise with result.
        WebIDL::resolve_promise(realm, promise, result);
    }));

    return promise;
}

// https://w3c.github.io/webcrypto/#SubtleCrypto-method-unwrapKey
GC::Ref<WebIDL::Promise> SubtleCrypto::unwrap_key(Bindings::KeyFormat format, KeyDataType wrapped_key, GC::Ref<CryptoKey> unwrapping_key, AlgorithmIdentifier algorithm, AlgorithmIdentifier unwrapped_key_algorithm, bool extractable, Vector<Bindings::KeyUsage> key_usages)
{
    auto& realm = this->realm();
    // 1. Let format, unwrappingKey, algorithm, unwrappedKeyAlgorithm, extractable and usages, be the format, unwrappingKey, unwrapAlgorithm,
    //    unwrappedKeyAlgorithm, extractable and keyUsages parameters passed to the unwrapKey() method, respectively.

    // 2. Let wrappedKey be the result of getting a copy of the bytes held by the wrappedKey parameter passed to the unwrapKey() method.
    auto real_wrapped_key = MUST(WebIDL::get_buffer_source_copy(*wrapped_key.get<GC::Root<WebIDL::BufferSource>>()->raw_object()));

    StringView operation;
    auto normalized_algorithm_or_error = [&]() -> WebIDL::ExceptionOr<NormalizedAlgorithmAndParameter> {
        // 3. Let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "unwrapKey".
        auto normalized_algorithm_unwrap_key_or_error = normalize_an_algorithm(realm, algorithm, "unwrapKey"_string);

        // 4. If an error occurred, let normalizedAlgorithm be the result of normalizing an algorithm, with alg set to algorithm and op set to "decrypt".
        // 5. If an error occurred, return a Promise rejected with normalizedAlgorithm.
        if (normalized_algorithm_unwrap_key_or_error.is_error()) {
            auto normalized_algorithm_decrypt_or_error = normalize_an_algorithm(realm, algorithm, "decrypt"_string);
            if (normalized_algorithm_decrypt_or_error.is_error())
                return normalized_algorithm_decrypt_or_error.release_error();

            operation = "decrypt"sv;
            return normalized_algorithm_decrypt_or_error.release_value();
        } else {
            operation = "unwrapKey"sv;
            return normalized_algorithm_unwrap_key_or_error.release_value();
        }
    }();
    if (normalized_algorithm_or_error.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_algorithm_or_error.release_error());

    auto normalized_algorithm = normalized_algorithm_or_error.release_value();

    // 6. Let normalizedKeyAlgorithm be the result of normalizing an algorithm, with alg set to unwrappedKeyAlgorithm and op set to "importKey".
    auto normalized_key_algorithm_or_error = normalize_an_algorithm(realm, unwrapped_key_algorithm, "importKey"_string);
    if (normalized_key_algorithm_or_error.is_error()) {
        // 7. If an error occurred, return a Promise rejected with normalizedKeyAlgorithm.
        return WebIDL::create_rejected_promise_from_exception(realm, normalized_key_algorithm_or_error.release_error());
    }

    auto normalized_key_algorithm = normalized_key_algorithm_or_error.release_value();

    // 8. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 9. Return promise and perform the remaining steps in parallel.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, normalized_algorithm = move(normalized_algorithm), promise, unwrapping_key = unwrapping_key, real_wrapped_key = move(real_wrapped_key), operation, format, extractable, key_usages = move(key_usages), normalized_key_algorithm = move(normalized_key_algorithm)]() mutable -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        // 10. If the following steps or referenced procedures say to throw an error, reject promise with the returned error and then terminate the algorithm.

        // 11. If the name member of normalizedAlgorithm is not equal to the name attribute of the [[algorithm]] internal slot
        //     of unwrappingKey then throw an InvalidAccessError.
        if (normalized_algorithm.parameter->name != unwrapping_key->algorithm_name()) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Algorithm mismatch"_string));
            return;
        }

        // 12. If the [[usages]] internal slot of unwrappingKey does not contain an entry that is "unwrapKey", then throw an InvalidAccessError.
        if (!unwrapping_key->internal_usages().contains_slow(Bindings::KeyUsage::Unwrapkey)) {
            WebIDL::reject_promise(realm, promise, WebIDL::InvalidAccessError::create(realm, "Key does not support unwrapping keys"_string));
            return;
        }

        auto key_or_error = [&]() -> WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> {
            // 13. If normalizedAlgorithm supports an unwrap key operation:
            if (operation == "unwrapKey") {
                // Let key be the result of performing the unwrap key operation specified by normalizedAlgorithm
                // using algorithm, unwrappingKey as key and wrappedKey as ciphertext.
                return normalized_algorithm.methods->unwrap_key(*normalized_algorithm.parameter, unwrapping_key, real_wrapped_key);
            }

            // Otherwise, if normalizedAlgorithm supports a decrypt operation:
            else if (operation == "decrypt") {
                // Let key be the result of performing the decrypt operation specified by normalizedAlgorithm
                // using algorithm, unwrappingKey as key and wrappedKey as ciphertext.
                return normalized_algorithm.methods->decrypt(*normalized_algorithm.parameter, unwrapping_key, real_wrapped_key);
            }

            // Otherwise:
            else {
                // throw a NotSupportedError.
                return WebIDL::NotSupportedError::create(realm, "Algorithm does not support wrapping"_string);
            }
        }();
        if (key_or_error.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), key_or_error.release_error()).release_value().value());
            return;
        }

        auto key = key_or_error.release_value();

        Variant<ByteBuffer, Bindings::JsonWebKey, Empty> bytes;

        // 14. If format is equal to the strings "raw", "pkcs8", or "spki":
        if (format == Bindings::KeyFormat::Raw || format == Bindings::KeyFormat::Pkcs8 || format == Bindings::KeyFormat::Spki) {
            // Set bytes be set to key.
            bytes = key->buffer();
        }

        // If format is equal to the string "jwk":
        else if (format == Bindings::KeyFormat::Jwk) {
            // Let bytes be the result of executing the parse a JWK algorithm, with key as the data to be parsed.
            auto maybe_parsed = Bindings::JsonWebKey::parse(realm, key->buffer());
            if (maybe_parsed.is_error()) {
                WebIDL::reject_promise(realm, promise, maybe_parsed.release_error().release_value().value());
                return;
            }

            bytes = maybe_parsed.release_value();
        } else {
            VERIFY_NOT_REACHED();
        }

        // 15. Let result be the result of performing the import key operation specified by normalizedKeyAlgorithm
        //    using unwrappedKeyAlgorithm as algorithm, format, usages and extractable and with bytes as keyData.
        auto result_or_error = normalized_key_algorithm.methods->import_key(*normalized_key_algorithm.parameter, format, bytes.downcast<CryptoKey::InternalKeyData>(), extractable, key_usages);
        if (result_or_error.is_error()) {
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), result_or_error.release_error()).release_value().value());
            return;
        }

        auto result = result_or_error.release_value();

        // 16. If the [[type]] internal slot of result is "secret" or "private" and usages is empty, then throw a SyntaxError.
        if ((result->type() == Bindings::KeyType::Secret || result->type() == Bindings::KeyType::Private) && key_usages.is_empty()) {
            WebIDL::reject_promise(realm, promise, WebIDL::SyntaxError::create(realm, "Usages must not be empty"_string));
            return;
        }

        // 17. Set the [[extractable]] internal slot of result to extractable.
        result->set_extractable(extractable);

        // 18. Set the [[usages]] internal slot of result to the normalized value of usages.
        normalize_key_usages(key_usages);
        result->set_usages(key_usages);

        // 19. Resolve promise with result.
        WebIDL::resolve_promise(realm, promise, result);
    }));

    return promise;
}

SupportedAlgorithmsMap& supported_algorithms_internal()
{
    static SupportedAlgorithmsMap s_supported_algorithms;
    return s_supported_algorithms;
}

// https://w3c.github.io/webcrypto/#algorithm-normalization-internalS
SupportedAlgorithmsMap const& supported_algorithms()
{
    auto& internal_object = supported_algorithms_internal();

    if (!internal_object.is_empty()) {
        return internal_object;
    }

    // 1. For each value, v in the List of supported operations,
    // set the v key of the internal object supportedAlgorithms to a new associative container.
    auto supported_operations = Vector {
        "encrypt"_string,
        "decrypt"_string,
        "sign"_string,
        "verify"_string,
        "digest"_string,
        "deriveBits"_string,
        "wrapKey"_string,
        "unwrapKey"_string,
        "generateKey"_string,
        "importKey"_string,
        "exportKey"_string,
        "get key length"_string,
    };

    for (auto& operation : supported_operations) {
        internal_object.set(operation, {});
    }

    // https://w3c.github.io/webcrypto/#algorithm-conventions

    // https://w3c.github.io/webcrypto/#rsassa-pkcs1-registration
    define_an_algorithm<RSASSAPKCS1>("sign"_string, "RSASSA-PKCS1-v1_5"_string);
    define_an_algorithm<RSASSAPKCS1>("verify"_string, "RSASSA-PKCS1-v1_5"_string);
    define_an_algorithm<RSASSAPKCS1, RsaHashedKeyGenParams>("generateKey"_string, "RSASSA-PKCS1-v1_5"_string);
    define_an_algorithm<RSASSAPKCS1, RsaHashedImportParams>("importKey"_string, "RSASSA-PKCS1-v1_5"_string);
    define_an_algorithm<RSASSAPKCS1>("exportKey"_string, "RSASSA-PKCS1-v1_5"_string);

    // https://w3c.github.io/webcrypto/#rsa-pss-registration
    define_an_algorithm<RSAPSS, RsaPssParams>("sign"_string, "RSA-PSS"_string);
    define_an_algorithm<RSAPSS, RsaPssParams>("verify"_string, "RSA-PSS"_string);
    define_an_algorithm<RSAPSS, RsaHashedKeyGenParams>("generateKey"_string, "RSA-PSS"_string);
    define_an_algorithm<RSAPSS, RsaHashedImportParams>("importKey"_string, "RSA-PSS"_string);
    define_an_algorithm<RSAPSS>("exportKey"_string, "RSA-PSS"_string);

    // https://w3c.github.io/webcrypto/#rsa-oaep-registration
    define_an_algorithm<RSAOAEP, RsaOaepParams>("encrypt"_string, "RSA-OAEP"_string);
    define_an_algorithm<RSAOAEP, RsaOaepParams>("decrypt"_string, "RSA-OAEP"_string);
    define_an_algorithm<RSAOAEP, RsaHashedKeyGenParams>("generateKey"_string, "RSA-OAEP"_string);
    define_an_algorithm<RSAOAEP, RsaHashedImportParams>("importKey"_string, "RSA-OAEP"_string);
    define_an_algorithm<RSAOAEP>("exportKey"_string, "RSA-OAEP"_string);

    // https://w3c.github.io/webcrypto/#ecdsa-registration
    define_an_algorithm<ECDSA, EcdsaParams>("sign"_string, "ECDSA"_string);
    define_an_algorithm<ECDSA, EcdsaParams>("verify"_string, "ECDSA"_string);
    define_an_algorithm<ECDSA, EcKeyGenParams>("generateKey"_string, "ECDSA"_string);
    define_an_algorithm<ECDSA, EcKeyImportParams>("importKey"_string, "ECDSA"_string);
    define_an_algorithm<ECDSA>("exportKey"_string, "ECDSA"_string);

    // https://w3c.github.io/webcrypto/#ecdh-registration
    define_an_algorithm<ECDH, EcKeyImportParams>("importKey"_string, "ECDH"_string);
    define_an_algorithm<ECDH>("exportKey"_string, "ECDH"_string);
    define_an_algorithm<ECDH, EcdhKeyDeriveParams>("deriveBits"_string, "ECDH"_string);
    define_an_algorithm<ECDH, EcKeyGenParams>("generateKey"_string, "ECDH"_string);

    // https://w3c.github.io/webcrypto/#aes-ctr-registration
    define_an_algorithm<AesCtr, AesCtrParams>("encrypt"_string, "AES-CTR"_string);
    define_an_algorithm<AesCtr, AesCtrParams>("decrypt"_string, "AES-CTR"_string);
    define_an_algorithm<AesCtr, AesKeyGenParams>("generateKey"_string, "AES-CTR"_string);
    define_an_algorithm<AesCtr>("importKey"_string, "AES-CTR"_string);
    define_an_algorithm<AesCtr>("exportKey"_string, "AES-CTR"_string);
    define_an_algorithm<AesCtr, AesDerivedKeyParams>("get key length"_string, "AES-CTR"_string);

    // https://w3c.github.io/webcrypto/#aes-cbc-registration
    define_an_algorithm<AesCbc, AesCbcParams>("encrypt"_string, "AES-CBC"_string);
    define_an_algorithm<AesCbc, AesCbcParams>("decrypt"_string, "AES-CBC"_string);
    define_an_algorithm<AesCbc, AesKeyGenParams>("generateKey"_string, "AES-CBC"_string);
    define_an_algorithm<AesCbc>("importKey"_string, "AES-CBC"_string);
    define_an_algorithm<AesCbc>("exportKey"_string, "AES-CBC"_string);
    define_an_algorithm<AesCbc, AesDerivedKeyParams>("get key length"_string, "AES-CBC"_string);

    // https://w3c.github.io/webcrypto/#aes-gcm-registration
    define_an_algorithm<AesGcm, AesGcmParams>("encrypt"_string, "AES-GCM"_string);
    define_an_algorithm<AesGcm, AesGcmParams>("decrypt"_string, "AES-GCM"_string);
    define_an_algorithm<AesGcm, AesKeyGenParams>("generateKey"_string, "AES-GCM"_string);
    define_an_algorithm<AesGcm>("importKey"_string, "AES-GCM"_string);
    define_an_algorithm<AesGcm>("exportKey"_string, "AES-GCM"_string);
    define_an_algorithm<AesGcm, AesDerivedKeyParams>("get key length"_string, "AES-GCM"_string);

    // https://w3c.github.io/webcrypto/#aes-kw-registration
    define_an_algorithm<AesKw>("wrapKey"_string, "AES-KW"_string);
    define_an_algorithm<AesKw>("unwrapKey"_string, "AES-KW"_string);
    define_an_algorithm<AesKw, AesKeyGenParams>("generateKey"_string, "AES-KW"_string);
    define_an_algorithm<AesKw>("importKey"_string, "AES-KW"_string);
    define_an_algorithm<AesKw>("exportKey"_string, "AES-KW"_string);
    define_an_algorithm<AesKw, AesDerivedKeyParams>("get key length"_string, "AES-KW"_string);

    // https://w3c.github.io/webcrypto/#hmac-registration
    define_an_algorithm<HMAC>("sign"_string, "HMAC"_string);
    define_an_algorithm<HMAC>("verify"_string, "HMAC"_string);
    define_an_algorithm<HMAC, HmacKeyGenParams>("generateKey"_string, "HMAC"_string);
    define_an_algorithm<HMAC, HmacImportParams>("importKey"_string, "HMAC"_string);
    define_an_algorithm<HMAC>("exportKey"_string, "HMAC"_string);
    define_an_algorithm<HMAC, HmacImportParams>("get key length"_string, "HMAC"_string);

    // https://w3c.github.io/webcrypto/#sha-registration
    define_an_algorithm<SHA>("digest"_string, "SHA-1"_string);
    define_an_algorithm<SHA>("digest"_string, "SHA-256"_string);
    define_an_algorithm<SHA>("digest"_string, "SHA-384"_string);
    define_an_algorithm<SHA>("digest"_string, "SHA-512"_string);

    // https://w3c.github.io/webcrypto/#hkdf-registration
    define_an_algorithm<HKDF, HKDFParams>("deriveBits"_string, "HKDF"_string);
    define_an_algorithm<HKDF>("importKey"_string, "HKDF"_string);
    define_an_algorithm<HKDF>("get key length"_string, "HKDF"_string);

    // https://w3c.github.io/webcrypto/#pbkdf2-registration
    define_an_algorithm<PBKDF2, PBKDF2Params>("deriveBits"_string, "PBKDF2"_string);
    define_an_algorithm<PBKDF2>("importKey"_string, "PBKDF2"_string);
    define_an_algorithm<PBKDF2>("get key length"_string, "PBKDF2"_string);

    // https://wicg.github.io/webcrypto-secure-curves/#x25519-registration
    define_an_algorithm<X25519, EcdhKeyDeriveParams>("deriveBits"_string, "X25519"_string);
    define_an_algorithm<X25519>("generateKey"_string, "X25519"_string);
    define_an_algorithm<X25519>("importKey"_string, "X25519"_string);
    define_an_algorithm<X25519>("exportKey"_string, "X25519"_string);

    // https://wicg.github.io/webcrypto-secure-curves/#x448-registration
    define_an_algorithm<X448, EcdhKeyDeriveParams>("deriveBits"_string, "X448"_string);
    define_an_algorithm<X448>("generateKey"_string, "X448"_string);
    define_an_algorithm<X448>("importKey"_string, "X448"_string);
    define_an_algorithm<X448>("exportKey"_string, "X448"_string);

    // https://wicg.github.io/webcrypto-secure-curves/#ed25519-registration
    define_an_algorithm<ED25519>("sign"_string, "Ed25519"_string);
    define_an_algorithm<ED25519>("verify"_string, "Ed25519"_string);
    define_an_algorithm<ED25519>("generateKey"_string, "Ed25519"_string);
    define_an_algorithm<ED25519>("importKey"_string, "Ed25519"_string);
    define_an_algorithm<ED25519>("exportKey"_string, "Ed25519"_string);

    // https://wicg.github.io/webcrypto-secure-curves/#ed448-registration
    define_an_algorithm<ED448, Ed448Params>("sign"_string, "Ed448"_string);
    define_an_algorithm<ED448, Ed448Params>("verify"_string, "Ed448"_string);
    define_an_algorithm<ED448>("generateKey"_string, "Ed448"_string);
    define_an_algorithm<ED448>("importKey"_string, "Ed448"_string);
    define_an_algorithm<ED448>("exportKey"_string, "Ed448"_string);

    return internal_object;
}

// https://w3c.github.io/webcrypto/#concept-define-an-algorithm
template<typename Methods, typename Param>
void define_an_algorithm(AK::String op, AK::String algorithm)
{
    auto& internal_object = supported_algorithms_internal();

    // 1. Let registeredAlgorithms be the associative container stored at the op key of supportedAlgorithms.
    // NOTE: There should always be a container at the op key.
    auto maybe_registered_algorithms = internal_object.get(op);
    auto registered_algorithms = maybe_registered_algorithms.value();

    // 2. Set the alg key of registeredAlgorithms to the IDL dictionary type type.
    registered_algorithms.set(algorithm, RegisteredAlgorithm { &Methods::create, &Param::from_value });
    internal_object.set(op, registered_algorithms);
}

}
