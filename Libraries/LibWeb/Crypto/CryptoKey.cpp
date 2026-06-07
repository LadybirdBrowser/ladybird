/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <AK/StdLibExtras.h>
#include <AK/Utf16String.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/CryptoKey.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/Crypto/CryptoKeySerializationTags.h>
#include <LibWeb/Crypto/KeyAlgorithms.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::Crypto {

namespace {

WebIDL::ExceptionOr<HandleTag> decode_handle_tag(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    auto tag = TRY(HTML::decode_or_throw_data_clone_error<u8>(realm, decoder));
    if (!handle_tag_is_in_table(static_cast<HandleTag>(tag)))
        return HTML::data_clone_error_from_serialization_error(realm, Error::from_string_literal("Invalid CryptoKey handle tag"));
    return static_cast<HandleTag>(tag);
}

WebIDL::ExceptionOr<KeyAlgorithmTag> decode_key_algorithm_tag(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    auto tag = TRY(HTML::decode_or_throw_data_clone_error<u8>(realm, decoder));
    if (!key_algorithm_tag_is_in_table(static_cast<KeyAlgorithmTag>(tag)))
        return HTML::data_clone_error_from_serialization_error(realm, Error::from_string_literal("Invalid CryptoKey algorithm tag"));
    return static_cast<KeyAlgorithmTag>(tag);
}

::Crypto::UnsignedBigInteger big_integer_from_api_big_integer(JS::Uint8Array const& big_integer)
{
    auto buffer = MUST(WebIDL::get_buffer_source_copy(big_integer));
    if (!buffer.is_empty())
        return ::Crypto::UnsignedBigInteger::import_data(buffer);
    return ::Crypto::UnsignedBigInteger(0);
}

void serialize_key_algorithm(HTML::StructuredSerializeWriter& encoder, JS::Object const& object)
{
    if (auto const* algorithm = as_if<RsaHashedKeyAlgorithm>(object)) {
        encoder.encode(to_underlying(KeyAlgorithmTag::RsaHashedKeyAlgorithm));
        encoder.encode(algorithm->name());
        encoder.encode(algorithm->modulus_length());
        HTML::encode_unsigned_big_integer(encoder, big_integer_from_api_big_integer(*algorithm->public_exponent()));
        encoder.encode(MUST(algorithm->hash().name(algorithm->vm())));
        return;
    }

    if (auto const* algorithm = as_if<RsaKeyAlgorithm>(object)) {
        encoder.encode(to_underlying(KeyAlgorithmTag::RsaKeyAlgorithm));
        encoder.encode(algorithm->name());
        encoder.encode(algorithm->modulus_length());
        HTML::encode_unsigned_big_integer(encoder, big_integer_from_api_big_integer(*algorithm->public_exponent()));
        return;
    }

    if (auto const* algorithm = as_if<EcKeyAlgorithm>(object)) {
        encoder.encode(to_underlying(KeyAlgorithmTag::EcKeyAlgorithm));
        encoder.encode(algorithm->name());
        encoder.encode(algorithm->named_curve());
        return;
    }

    if (auto const* algorithm = as_if<AesKeyAlgorithm>(object)) {
        encoder.encode(to_underlying(KeyAlgorithmTag::AesKeyAlgorithm));
        encoder.encode(algorithm->name());
        encoder.encode(algorithm->length());
        return;
    }

    if (auto const* algorithm = as_if<HmacKeyAlgorithm>(object)) {
        encoder.encode(to_underlying(KeyAlgorithmTag::HmacKeyAlgorithm));
        encoder.encode(algorithm->name());
        encoder.encode(algorithm->hash()->name());
        encoder.encode(algorithm->length());
        return;
    }

    if (auto const* algorithm = as_if<KmacKeyAlgorithm>(object)) {
        encoder.encode(to_underlying(KeyAlgorithmTag::KmacKeyAlgorithm));
        encoder.encode(algorithm->name());
        encoder.encode(algorithm->length());
        return;
    }

    auto const& algorithm = as<KeyAlgorithm>(object);
    encoder.encode(to_underlying(KeyAlgorithmTag::KeyAlgorithm));
    encoder.encode(algorithm.name());
}

WebIDL::ExceptionOr<GC::Ref<JS::Object>> deserialize_key_algorithm(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    auto tag = TRY(decode_key_algorithm_tag(decoder, realm));
    switch (tag) {
    case KeyAlgorithmTag::KeyAlgorithm: {
        auto algorithm = KeyAlgorithm::create(realm);
        algorithm->set_name(TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, decoder)));
        return algorithm;
    }
    case KeyAlgorithmTag::RsaKeyAlgorithm: {
        auto algorithm = RsaKeyAlgorithm::create(realm);
        algorithm->set_name(TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, decoder)));
        algorithm->set_modulus_length(TRY(HTML::decode_or_throw_data_clone_error<u32>(realm, decoder)));
        TRY(algorithm->set_public_exponent(TRY(HTML::decode_unsigned_big_integer(decoder, realm))));
        return algorithm;
    }
    case KeyAlgorithmTag::RsaHashedKeyAlgorithm: {
        auto algorithm = RsaHashedKeyAlgorithm::create(realm);
        algorithm->set_name(TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, decoder)));
        algorithm->set_modulus_length(TRY(HTML::decode_or_throw_data_clone_error<u32>(realm, decoder)));
        TRY(algorithm->set_public_exponent(TRY(HTML::decode_unsigned_big_integer(decoder, realm))));
        algorithm->set_hash(TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, decoder)));
        return algorithm;
    }
    case KeyAlgorithmTag::EcKeyAlgorithm: {
        auto algorithm = EcKeyAlgorithm::create(realm);
        algorithm->set_name(TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, decoder)));
        algorithm->set_named_curve(TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, decoder)));
        return algorithm;
    }
    case KeyAlgorithmTag::AesKeyAlgorithm: {
        auto algorithm = AesKeyAlgorithm::create(realm);
        algorithm->set_name(TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, decoder)));
        algorithm->set_length(TRY(HTML::decode_or_throw_data_clone_error<u16>(realm, decoder)));
        return algorithm;
    }
    case KeyAlgorithmTag::HmacKeyAlgorithm: {
        auto algorithm = HmacKeyAlgorithm::create(realm);
        algorithm->set_name(TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, decoder)));
        auto hash = KeyAlgorithm::create(realm);
        hash->set_name(TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, decoder)));
        algorithm->set_hash(hash);
        algorithm->set_length(TRY(HTML::decode_or_throw_data_clone_error<WebIDL::UnsignedLong>(realm, decoder)));
        return algorithm;
    }
    case KeyAlgorithmTag::KmacKeyAlgorithm: {
        auto algorithm = KmacKeyAlgorithm::create(realm);
        algorithm->set_name(TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, decoder)));
        algorithm->set_length(TRY(HTML::decode_or_throw_data_clone_error<WebIDL::UnsignedLong>(realm, decoder)));
        return algorithm;
    }
    }
    VERIFY_NOT_REACHED();
}

void serialize_handle(HTML::StructuredSerializeWriter& encoder, ByteBuffer const& buffer)
{
    encoder.encode(to_underlying(HandleTag::ByteBuffer));
    encoder.encode(buffer);
}

void serialize_handle(HTML::StructuredSerializeWriter& encoder, ::Crypto::PK::RSAPublicKey const& key)
{
    encoder.encode(to_underlying(HandleTag::RSAPublicKey));
    HTML::encode_unsigned_big_integer(encoder, key.modulus());
    HTML::encode_unsigned_big_integer(encoder, key.public_exponent());
}

WebIDL::ExceptionOr<::Crypto::PK::RSAPublicKey> deserialize_rsa_public_key(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    auto modulus = TRY(HTML::decode_unsigned_big_integer(decoder, realm));
    auto public_exponent = TRY(HTML::decode_unsigned_big_integer(decoder, realm));
    return ::Crypto::PK::RSAPublicKey { move(modulus), move(public_exponent) };
}

void serialize_handle(HTML::StructuredSerializeWriter& encoder, ::Crypto::PK::RSAPrivateKey const& key)
{
    encoder.encode(to_underlying(HandleTag::RSAPrivateKey));
    HTML::encode_unsigned_big_integer(encoder, key.modulus());
    HTML::encode_unsigned_big_integer(encoder, key.private_exponent());
    HTML::encode_unsigned_big_integer(encoder, key.public_exponent());
    HTML::encode_unsigned_big_integer(encoder, key.prime1());
    HTML::encode_unsigned_big_integer(encoder, key.prime2());
    HTML::encode_unsigned_big_integer(encoder, key.exponent1());
    HTML::encode_unsigned_big_integer(encoder, key.exponent2());
    HTML::encode_unsigned_big_integer(encoder, key.coefficient());
}

WebIDL::ExceptionOr<::Crypto::PK::RSAPrivateKey> deserialize_rsa_private_key(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    auto modulus = TRY(HTML::decode_unsigned_big_integer(decoder, realm));
    auto private_exponent = TRY(HTML::decode_unsigned_big_integer(decoder, realm));
    auto public_exponent = TRY(HTML::decode_unsigned_big_integer(decoder, realm));
    auto prime1 = TRY(HTML::decode_unsigned_big_integer(decoder, realm));
    auto prime2 = TRY(HTML::decode_unsigned_big_integer(decoder, realm));
    auto exponent1 = TRY(HTML::decode_unsigned_big_integer(decoder, realm));
    auto exponent2 = TRY(HTML::decode_unsigned_big_integer(decoder, realm));
    auto coefficient = TRY(HTML::decode_unsigned_big_integer(decoder, realm));
    return ::Crypto::PK::RSAPrivateKey { move(modulus), move(private_exponent), move(public_exponent), move(prime1), move(prime2), move(exponent1), move(exponent2), move(coefficient) };
}

void serialize_ec_public_key(HTML::StructuredSerializeWriter& encoder, ::Crypto::PK::ECPublicKey const& key)
{
    encoder.encode(MUST(key.x_bytes()));
    encoder.encode(MUST(key.y_bytes()));
}

void serialize_handle(HTML::StructuredSerializeWriter& encoder, ::Crypto::PK::ECPublicKey const& key)
{
    encoder.encode(to_underlying(HandleTag::ECPublicKey));
    serialize_ec_public_key(encoder, key);
}

WebIDL::ExceptionOr<::Crypto::PK::ECPublicKey> deserialize_ec_public_key(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    auto x_bytes = TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder));
    auto y_bytes = TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder));
    auto scalar_size = x_bytes.size();
    return ::Crypto::PK::ECPublicKey { ::Crypto::UnsignedBigInteger::import_data(x_bytes), ::Crypto::UnsignedBigInteger::import_data(y_bytes), scalar_size };
}

void serialize_handle(HTML::StructuredSerializeWriter& encoder, ::Crypto::PK::ECPrivateKey const& key)
{
    encoder.encode(to_underlying(HandleTag::ECPrivateKey));
    encoder.encode(MUST(key.d_bytes()));
    encoder.encode(key.parameters());
    encoder.encode(key.public_key().has_value());
    if (key.public_key().has_value())
        serialize_ec_public_key(encoder, *key.public_key());
}

WebIDL::ExceptionOr<::Crypto::PK::ECPrivateKey> deserialize_ec_private_key(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    auto d_bytes = TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder));
    auto scalar_size = d_bytes.size();
    auto d = ::Crypto::UnsignedBigInteger::import_data(d_bytes);

    auto parameters = TRY(HTML::decode_or_throw_data_clone_error<Optional<Vector<int>>>(realm, decoder));

    Optional<::Crypto::PK::ECPublicKey> public_key;
    if (TRY(HTML::decode_or_throw_data_clone_error<bool>(realm, decoder)))
        public_key = TRY(deserialize_ec_public_key(decoder, realm));

    return ::Crypto::PK::ECPrivateKey { move(d), scalar_size, move(parameters), move(public_key) };
}

void serialize_handle(HTML::StructuredSerializeWriter& encoder, ::Crypto::PK::MLDSAPublicKey const& key)
{
    encoder.encode(to_underlying(HandleTag::MLDSAPublicKey));
    encoder.encode(key.public_key());
}

WebIDL::ExceptionOr<::Crypto::PK::MLDSAPublicKey> deserialize_mldsa_public_key(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    return ::Crypto::PK::MLDSAPublicKey { TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder)) };
}

void serialize_handle(HTML::StructuredSerializeWriter& encoder, ::Crypto::PK::MLDSAPrivateKey const& key)
{
    encoder.encode(to_underlying(HandleTag::MLDSAPrivateKey));
    encoder.encode(key.seed());
    encoder.encode(key.public_key());
    encoder.encode(key.private_key());
}

WebIDL::ExceptionOr<::Crypto::PK::MLDSAPrivateKey> deserialize_mldsa_private_key(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    auto seed = TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder));
    auto public_key = TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder));
    auto private_key = TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder));
    return ::Crypto::PK::MLDSAPrivateKey { move(seed), move(public_key), move(private_key) };
}

void serialize_handle(HTML::StructuredSerializeWriter& encoder, ::Crypto::PK::MLKEMPublicKey const& key)
{
    encoder.encode(to_underlying(HandleTag::MLKEMPublicKey));
    encoder.encode(key.public_key());
}

WebIDL::ExceptionOr<::Crypto::PK::MLKEMPublicKey> deserialize_mlkem_public_key(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    return ::Crypto::PK::MLKEMPublicKey { TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder)) };
}

void serialize_handle(HTML::StructuredSerializeWriter& encoder, ::Crypto::PK::MLKEMPrivateKey const& key)
{
    encoder.encode(to_underlying(HandleTag::MLKEMPrivateKey));
    encoder.encode(key.seed());
    encoder.encode(key.public_key());
    encoder.encode(key.private_key());
}

WebIDL::ExceptionOr<::Crypto::PK::MLKEMPrivateKey> deserialize_mlkem_private_key(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    auto seed = TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder));
    auto public_key = TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder));
    auto private_key = TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder));
    return ::Crypto::PK::MLKEMPrivateKey { move(seed), move(public_key), move(private_key) };
}

void serialize_handle(HTML::StructuredSerializeWriter& encoder, OKPPublicKey const& key)
{
    encoder.encode(to_underlying(HandleTag::OKPPublicKey));
    encoder.encode(key.bytes);
}

WebIDL::ExceptionOr<OKPPublicKey> deserialize_okp_public_key(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    return OKPPublicKey { TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder)) };
}

void serialize_handle(HTML::StructuredSerializeWriter& encoder, OKPPrivateKey const& key)
{
    encoder.encode(to_underlying(HandleTag::OKPPrivateKey));
    encoder.encode(key.bytes);
}

WebIDL::ExceptionOr<OKPPrivateKey> deserialize_okp_private_key(HTML::StructuredSerializeReader& decoder, JS::Realm& realm)
{
    return OKPPrivateKey { TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, decoder)) };
}

}

GC_DEFINE_ALLOCATOR(CryptoKey);
GC_DEFINE_ALLOCATOR(CryptoKeyPair);

GC::Ref<CryptoKey> CryptoKey::create(JS::Realm& realm, InternalKeyData key_data)
{
    return realm.create<CryptoKey>(realm, move(key_data));
}

GC::Ref<CryptoKey> CryptoKey::create(JS::Realm& realm)
{
    return realm.create<CryptoKey>(realm);
}

CryptoKey::CryptoKey(JS::Realm& realm, InternalKeyData key_data)
    : PlatformObject(realm)
    , m_algorithm_cached(Object::create(realm, nullptr))
    , m_usages_cached(Object::create(realm, nullptr))
    , m_key_data(move(key_data))
{
}

CryptoKey::CryptoKey(JS::Realm& realm)
    : PlatformObject(realm)
    , m_algorithm_cached(Object::create(realm, nullptr))
    , m_usages_cached(Object::create(realm, nullptr))
    , m_key_data(MUST(ByteBuffer::create_uninitialized(0)))
{
}

void CryptoKey::finalize()
{
    Base::finalize();
    m_key_data.visit(
        [](ByteBuffer& data) { secure_zero(data.data(), data.size()); },
        [](OKPPublicKey& data) { secure_zero(data.bytes.data(), data.bytes.size()); },
        [](OKPPrivateKey& data) { secure_zero(data.bytes.data(), data.bytes.size()); },
        [](auto& data) { secure_zero(reinterpret_cast<u8*>(&data), sizeof(data)); });
}

void CryptoKey::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CryptoKey);
    Base::initialize(realm);
}

void CryptoKey::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_algorithm_cached);
    visitor.visit(m_usages_cached);
}

void CryptoKey::set_usages(Vector<Bindings::KeyUsage> usages)
{
    m_usages = move(usages);
    auto& realm = this->realm();
    m_usages_cached = JS::Array::create_from<Bindings::KeyUsage>(realm, m_usages.span(), [&](auto& key_usage) -> JS::Value {
        return JS::PrimitiveString::create(realm.vm(), Bindings::idl_enum_to_string(key_usage));
    });
}

Utf16String const& CryptoKey::algorithm_name() const
{
    if (m_algorithm_name.is_empty()) {
        auto name = MUST(m_algorithm_cached->get("name"_utf16_fly_string));
        m_algorithm_name = MUST(name.to_utf16_string(vm()));
    }
    return m_algorithm_name;
}

GC::Ref<CryptoKeyPair> CryptoKeyPair::create(JS::Realm& realm, GC::Ref<CryptoKey> public_key, GC::Ref<CryptoKey> private_key)
{
    return realm.create<CryptoKeyPair>(realm, public_key, private_key);
}

CryptoKeyPair::CryptoKeyPair(JS::Realm& realm, GC::Ref<CryptoKey> public_key, GC::Ref<CryptoKey> private_key)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
    , m_public_key(public_key)
    , m_private_key(private_key)
{
}

void CryptoKeyPair::initialize(JS::Realm& realm)
{
    define_native_accessor(realm, "publicKey"_utf16_fly_string, public_key_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
    define_native_accessor(realm, "privateKey"_utf16_fly_string, private_key_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);

    Base::initialize(realm);
}

void CryptoKeyPair::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_public_key);
    visitor.visit(m_private_key);
}

static JS::ThrowCompletionOr<CryptoKeyPair*> impl_from(JS::VM& vm)
{
    auto this_value = vm.this_value();
    JS::Object* this_object = nullptr;
    if (this_value.is_nullish())
        this_object = &vm.current_realm()->global_object();
    else
        this_object = TRY(this_value.to_object(vm));

    auto* crypto_key_pair = as_if<CryptoKeyPair>(this_object);
    if (!crypto_key_pair)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "CryptoKeyPair");
    return crypto_key_pair;
}

JS_DEFINE_NATIVE_FUNCTION(CryptoKeyPair::public_key_getter)
{
    auto* impl = TRY(impl_from(vm));
    return TRY(Bindings::throw_dom_exception_if_needed(vm, [&] { return impl->public_key(); }));
}

JS_DEFINE_NATIVE_FUNCTION(CryptoKeyPair::private_key_getter)
{
    auto* impl = TRY(impl_from(vm));
    return TRY(Bindings::throw_dom_exception_if_needed(vm, [&] { return impl->private_key(); }));
}

WebIDL::ExceptionOr<void> CryptoKey::serialization_steps(HTML::StructuredSerializeWriter& serialized, bool, HTML::SerializationMemory&)
{
    // 1. Set serialized.[[Type]] to the [[type]] internal slot of value.
    serialized.encode(m_type);

    // 2. Set serialized.[[Extractable]] to the [[extractable]] internal slot of value.
    serialized.encode(m_extractable);

    // 3. Set serialized.[[Algorithm]] to the sub-serialization of the [[algorithm]] internal slot of value.
    serialize_key_algorithm(serialized, m_algorithm_cached);

    // 4. Set serialized.[[Usages]] to the sub-serialization of the [[usages]] internal slot of value.
    serialized.encode(m_usages);

    // 5. Set serialized.[[Handle]] to the [[handle]] internal slot of value.
    m_key_data.visit([&](auto const& handle) { serialize_handle(serialized, handle); });

    return {};
}

WebIDL::ExceptionOr<void> CryptoKey::deserialization_steps(HTML::StructuredSerializeReader& serialized, HTML::DeserializationMemory&)
{
    auto& realm = this->realm();

    // 1. Initialize the [[type]] internal slot of value to serialized.[[Type]].
    m_type = TRY(HTML::decode_or_throw_data_clone_error<Bindings::KeyType>(realm, serialized));

    // 2. Initialize the [[extractable]] internal slot of value to serialized.[[Extractable]].
    m_extractable = TRY(HTML::decode_or_throw_data_clone_error<bool>(realm, serialized));

    // 3. Initialize the [[algorithm]] internal slot of value to the sub-deserialization of serialized.[[Algorithm]].
    m_algorithm_cached = TRY(deserialize_key_algorithm(serialized, realm));

    // 4. Initialize the [[usages]] internal slot of value to the sub-deserialization of serialized.[[Usages]].
    set_usages(TRY(HTML::decode_or_throw_data_clone_error<Vector<Bindings::KeyUsage>>(realm, serialized)));

    // 5. Initialize the [[handle]] internal slot of value to serialized.[[Handle]].
    auto tag = TRY(decode_handle_tag(serialized, realm));
    switch (tag) {
    case HandleTag::ByteBuffer:
        m_key_data = TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, serialized));
        break;
    case HandleTag::RSAPublicKey:
        m_key_data = TRY(deserialize_rsa_public_key(serialized, realm));
        break;
    case HandleTag::RSAPrivateKey:
        m_key_data = TRY(deserialize_rsa_private_key(serialized, realm));
        break;
    case HandleTag::ECPublicKey:
        m_key_data = TRY(deserialize_ec_public_key(serialized, realm));
        break;
    case HandleTag::ECPrivateKey:
        m_key_data = TRY(deserialize_ec_private_key(serialized, realm));
        break;
    case HandleTag::MLDSAPublicKey:
        m_key_data = TRY(deserialize_mldsa_public_key(serialized, realm));
        break;
    case HandleTag::MLDSAPrivateKey:
        m_key_data = TRY(deserialize_mldsa_private_key(serialized, realm));
        break;
    case HandleTag::MLKEMPublicKey:
        m_key_data = TRY(deserialize_mlkem_public_key(serialized, realm));
        break;
    case HandleTag::MLKEMPrivateKey:
        m_key_data = TRY(deserialize_mlkem_private_key(serialized, realm));
        break;
    case HandleTag::OKPPublicKey:
        m_key_data = TRY(deserialize_okp_public_key(serialized, realm));
        break;
    case HandleTag::OKPPrivateKey:
        m_key_data = TRY(deserialize_okp_private_key(serialized, realm));
        break;
    }

    return {};
}

}
