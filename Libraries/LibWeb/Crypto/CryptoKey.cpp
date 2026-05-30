/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/CryptoKey.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/Crypto/KeyAlgorithms.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::Crypto {

namespace {

enum class HandleTag : u8 {
    ByteBuffer = 0,
    JsonWebKey = 1,
    RSAPublicKey = 2,
    RSAPrivateKey = 3,
    ECPublicKey = 4,
    ECPrivateKey = 5,
    MLDSAPublicKey = 6,
    MLDSAPrivateKey = 7,
    MLKEMPublicKey = 8,
    MLKEMPrivateKey = 9,
};

enum class KeyAlgorithmTag : u8 {
    KeyAlgorithm = 0,
    RsaKeyAlgorithm = 1,
    RsaHashedKeyAlgorithm = 2,
    EcKeyAlgorithm = 3,
    AesKeyAlgorithm = 4,
    HmacKeyAlgorithm = 5,
    KmacKeyAlgorithm = 6,
};

::Crypto::UnsignedBigInteger big_integer_from_api_big_integer(JS::Uint8Array const& big_integer)
{
    auto buffer = big_integer.viewed_array_buffer()->bytes().slice(big_integer.byte_offset(), big_integer.byte_length().length());
    if (!buffer.is_empty())
        return ::Crypto::UnsignedBigInteger::import_data(buffer);
    return ::Crypto::UnsignedBigInteger(0);
}

void serialize_key_algorithm(HTML::TransferDataEncoder& encoder, JS::Object const& object)
{
    if (auto const* algorithm = as_if<RsaHashedKeyAlgorithm>(object)) {
        encoder.encode(KeyAlgorithmTag::RsaHashedKeyAlgorithm);
        encoder.encode(algorithm->name());
        encoder.encode(algorithm->modulus_length());
        encoder.encode_unsigned_big_integer(big_integer_from_api_big_integer(*algorithm->public_exponent()));
        encoder.encode(MUST(algorithm->hash().name(algorithm->vm())));
        return;
    }

    if (auto const* algorithm = as_if<RsaKeyAlgorithm>(object)) {
        encoder.encode(KeyAlgorithmTag::RsaKeyAlgorithm);
        encoder.encode(algorithm->name());
        encoder.encode(algorithm->modulus_length());
        encoder.encode_unsigned_big_integer(big_integer_from_api_big_integer(*algorithm->public_exponent()));
        return;
    }

    if (auto const* algorithm = as_if<EcKeyAlgorithm>(object)) {
        encoder.encode(KeyAlgorithmTag::EcKeyAlgorithm);
        encoder.encode(algorithm->name());
        encoder.encode(algorithm->named_curve());
        return;
    }

    if (auto const* algorithm = as_if<AesKeyAlgorithm>(object)) {
        encoder.encode(KeyAlgorithmTag::AesKeyAlgorithm);
        encoder.encode(algorithm->name());
        encoder.encode(algorithm->length());
        return;
    }

    if (auto const* algorithm = as_if<HmacKeyAlgorithm>(object)) {
        encoder.encode(KeyAlgorithmTag::HmacKeyAlgorithm);
        encoder.encode(algorithm->name());
        encoder.encode(algorithm->hash()->name());
        encoder.encode(algorithm->length());
        return;
    }

    if (auto const* algorithm = as_if<KmacKeyAlgorithm>(object)) {
        encoder.encode(KeyAlgorithmTag::KmacKeyAlgorithm);
        encoder.encode(algorithm->name());
        encoder.encode(algorithm->length());
        return;
    }

    auto const& algorithm = as<KeyAlgorithm>(object);
    encoder.encode(KeyAlgorithmTag::KeyAlgorithm);
    encoder.encode(algorithm.name());
}

WebIDL::ExceptionOr<GC::Ref<JS::Object>> deserialize_key_algorithm(HTML::TransferDataDecoder& decoder, JS::Realm& realm)
{
    auto tag = decoder.decode<KeyAlgorithmTag>();
    switch (tag) {
    case KeyAlgorithmTag::KeyAlgorithm: {
        auto algorithm = KeyAlgorithm::create(realm);
        algorithm->set_name(decoder.decode<String>());
        return algorithm;
    }
    case KeyAlgorithmTag::RsaKeyAlgorithm: {
        auto algorithm = RsaKeyAlgorithm::create(realm);
        algorithm->set_name(decoder.decode<String>());
        algorithm->set_modulus_length(decoder.decode<u32>());
        TRY(algorithm->set_public_exponent(TRY(decoder.decode_unsigned_big_integer(realm))));
        return algorithm;
    }
    case KeyAlgorithmTag::RsaHashedKeyAlgorithm: {
        auto algorithm = RsaHashedKeyAlgorithm::create(realm);
        algorithm->set_name(decoder.decode<String>());
        algorithm->set_modulus_length(decoder.decode<u32>());
        TRY(algorithm->set_public_exponent(TRY(decoder.decode_unsigned_big_integer(realm))));
        algorithm->set_hash(decoder.decode<String>());
        return algorithm;
    }
    case KeyAlgorithmTag::EcKeyAlgorithm: {
        auto algorithm = EcKeyAlgorithm::create(realm);
        algorithm->set_name(decoder.decode<String>());
        algorithm->set_named_curve(decoder.decode<String>());
        return algorithm;
    }
    case KeyAlgorithmTag::AesKeyAlgorithm: {
        auto algorithm = AesKeyAlgorithm::create(realm);
        algorithm->set_name(decoder.decode<String>());
        algorithm->set_length(decoder.decode<u16>());
        return algorithm;
    }
    case KeyAlgorithmTag::HmacKeyAlgorithm: {
        auto algorithm = HmacKeyAlgorithm::create(realm);
        algorithm->set_name(decoder.decode<String>());
        auto hash = KeyAlgorithm::create(realm);
        hash->set_name(decoder.decode<String>());
        algorithm->set_hash(hash);
        algorithm->set_length(decoder.decode<WebIDL::UnsignedLong>());
        return algorithm;
    }
    case KeyAlgorithmTag::KmacKeyAlgorithm: {
        auto algorithm = KmacKeyAlgorithm::create(realm);
        algorithm->set_name(decoder.decode<String>());
        algorithm->set_length(decoder.decode<WebIDL::UnsignedLong>());
        return algorithm;
    }
    }
    VERIFY_NOT_REACHED();
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ByteBuffer const& buffer)
{
    encoder.encode(HandleTag::ByteBuffer);
    encoder.encode(buffer);
}

void serialize_handle(HTML::TransferDataEncoder& encoder, RsaOtherPrimesInfo const& info)
{
    encoder.encode(info.r);
    encoder.encode(info.d);
    encoder.encode(info.t);
}

RsaOtherPrimesInfo deserialize_rsa_other_primes_info(HTML::TransferDataDecoder& decoder)
{
    return RsaOtherPrimesInfo {
        .r = decoder.decode<Optional<String>>(),
        .d = decoder.decode<Optional<String>>(),
        .t = decoder.decode<Optional<String>>(),
    };
}

void serialize_handle(HTML::TransferDataEncoder& encoder, JsonWebKey const& jwk)
{
    encoder.encode(HandleTag::JsonWebKey);
    encoder.encode(jwk.kty);
    encoder.encode(jwk.use);
    encoder.encode(jwk.key_ops);
    encoder.encode(jwk.alg);
    encoder.encode(jwk.ext);
    encoder.encode(jwk.crv);
    encoder.encode(jwk.x);
    encoder.encode(jwk.y);
    encoder.encode(jwk.d);
    encoder.encode(jwk.n);
    encoder.encode(jwk.e);
    encoder.encode(jwk.p);
    encoder.encode(jwk.q);
    encoder.encode(jwk.dp);
    encoder.encode(jwk.dq);
    encoder.encode(jwk.qi);
    encoder.encode(jwk.oth.has_value());
    if (jwk.oth.has_value()) {
        encoder.encode(static_cast<u64>(jwk.oth->size()));
        for (auto const& info : *jwk.oth)
            serialize_handle(encoder, info);
    }
    encoder.encode(jwk.k);
    encoder.encode(jwk.pub);
    encoder.encode(jwk.priv);
}

JsonWebKey deserialize_json_web_key(HTML::TransferDataDecoder& decoder)
{
    JsonWebKey jwk;
    jwk.kty = decoder.decode<Optional<String>>();
    jwk.use = decoder.decode<Optional<String>>();
    jwk.key_ops = decoder.decode<Optional<Vector<String>>>();
    jwk.alg = decoder.decode<Optional<String>>();
    jwk.ext = decoder.decode<Optional<bool>>();
    jwk.crv = decoder.decode<Optional<String>>();
    jwk.x = decoder.decode<Optional<String>>();
    jwk.y = decoder.decode<Optional<String>>();
    jwk.d = decoder.decode<Optional<String>>();
    jwk.n = decoder.decode<Optional<String>>();
    jwk.e = decoder.decode<Optional<String>>();
    jwk.p = decoder.decode<Optional<String>>();
    jwk.q = decoder.decode<Optional<String>>();
    jwk.dp = decoder.decode<Optional<String>>();
    jwk.dq = decoder.decode<Optional<String>>();
    jwk.qi = decoder.decode<Optional<String>>();
    if (decoder.decode<bool>()) {
        auto size = decoder.decode<u64>();
        Vector<RsaOtherPrimesInfo> oth;
        oth.ensure_capacity(size);
        for (u64 i = 0; i < size; ++i)
            oth.unchecked_append(deserialize_rsa_other_primes_info(decoder));
        jwk.oth = move(oth);
    }
    jwk.k = decoder.decode<Optional<String>>();
    jwk.pub = decoder.decode<Optional<String>>();
    jwk.priv = decoder.decode<Optional<String>>();
    return jwk;
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ::Crypto::PK::RSAPublicKey const& key)
{
    encoder.encode(HandleTag::RSAPublicKey);
    encoder.encode_unsigned_big_integer(key.modulus());
    encoder.encode_unsigned_big_integer(key.public_exponent());
}

WebIDL::ExceptionOr<::Crypto::PK::RSAPublicKey> deserialize_rsa_public_key(HTML::TransferDataDecoder& decoder, JS::Realm& realm)
{
    auto modulus = TRY(decoder.decode_unsigned_big_integer(realm));
    auto public_exponent = TRY(decoder.decode_unsigned_big_integer(realm));
    return ::Crypto::PK::RSAPublicKey { move(modulus), move(public_exponent) };
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ::Crypto::PK::RSAPrivateKey const& key)
{
    encoder.encode(HandleTag::RSAPrivateKey);
    encoder.encode_unsigned_big_integer(key.modulus());
    encoder.encode_unsigned_big_integer(key.private_exponent());
    encoder.encode_unsigned_big_integer(key.public_exponent());
    encoder.encode_unsigned_big_integer(key.prime1());
    encoder.encode_unsigned_big_integer(key.prime2());
    encoder.encode_unsigned_big_integer(key.exponent1());
    encoder.encode_unsigned_big_integer(key.exponent2());
    encoder.encode_unsigned_big_integer(key.coefficient());
}

WebIDL::ExceptionOr<::Crypto::PK::RSAPrivateKey> deserialize_rsa_private_key(HTML::TransferDataDecoder& decoder, JS::Realm& realm)
{
    auto modulus = TRY(decoder.decode_unsigned_big_integer(realm));
    auto private_exponent = TRY(decoder.decode_unsigned_big_integer(realm));
    auto public_exponent = TRY(decoder.decode_unsigned_big_integer(realm));
    auto prime1 = TRY(decoder.decode_unsigned_big_integer(realm));
    auto prime2 = TRY(decoder.decode_unsigned_big_integer(realm));
    auto exponent1 = TRY(decoder.decode_unsigned_big_integer(realm));
    auto exponent2 = TRY(decoder.decode_unsigned_big_integer(realm));
    auto coefficient = TRY(decoder.decode_unsigned_big_integer(realm));
    return ::Crypto::PK::RSAPrivateKey { move(modulus), move(private_exponent), move(public_exponent), move(prime1), move(prime2), move(exponent1), move(exponent2), move(coefficient) };
}

void serialize_ec_public_key(HTML::TransferDataEncoder& encoder, ::Crypto::PK::ECPublicKey const& key)
{
    encoder.encode(MUST(key.x_bytes()));
    encoder.encode(MUST(key.y_bytes()));
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ::Crypto::PK::ECPublicKey const& key)
{
    encoder.encode(HandleTag::ECPublicKey);
    serialize_ec_public_key(encoder, key);
}

WebIDL::ExceptionOr<::Crypto::PK::ECPublicKey> deserialize_ec_public_key(HTML::TransferDataDecoder& decoder, JS::Realm& realm)
{
    auto x_bytes = TRY(decoder.decode_buffer(realm));
    auto y_bytes = TRY(decoder.decode_buffer(realm));
    auto scalar_size = x_bytes.size();
    return ::Crypto::PK::ECPublicKey { ::Crypto::UnsignedBigInteger::import_data(x_bytes), ::Crypto::UnsignedBigInteger::import_data(y_bytes), scalar_size };
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ::Crypto::PK::ECPrivateKey const& key)
{
    encoder.encode(HandleTag::ECPrivateKey);
    encoder.encode(MUST(key.d_bytes()));
    encoder.encode(key.parameters());
    encoder.encode(key.public_key().has_value());
    if (key.public_key().has_value())
        serialize_ec_public_key(encoder, *key.public_key());
}

WebIDL::ExceptionOr<::Crypto::PK::ECPrivateKey> deserialize_ec_private_key(HTML::TransferDataDecoder& decoder, JS::Realm& realm)
{
    auto d_bytes = TRY(decoder.decode_buffer(realm));
    auto scalar_size = d_bytes.size();
    auto d = ::Crypto::UnsignedBigInteger::import_data(d_bytes);
    auto parameters = decoder.decode<Optional<Vector<int>>>();

    Optional<::Crypto::PK::ECPublicKey> public_key;
    if (decoder.decode<bool>())
        public_key = TRY(deserialize_ec_public_key(decoder, realm));

    return ::Crypto::PK::ECPrivateKey { move(d), scalar_size, move(parameters), move(public_key) };
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ::Crypto::PK::MLDSAPublicKey const& key)
{
    encoder.encode(HandleTag::MLDSAPublicKey);
    encoder.encode(key.public_key());
}

WebIDL::ExceptionOr<::Crypto::PK::MLDSAPublicKey> deserialize_mldsa_public_key(HTML::TransferDataDecoder& decoder, JS::Realm& realm)
{
    return ::Crypto::PK::MLDSAPublicKey { TRY(decoder.decode_buffer(realm)) };
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ::Crypto::PK::MLDSAPrivateKey const& key)
{
    encoder.encode(HandleTag::MLDSAPrivateKey);
    encoder.encode(key.seed());
    encoder.encode(key.public_key());
    encoder.encode(key.private_key());
}

WebIDL::ExceptionOr<::Crypto::PK::MLDSAPrivateKey> deserialize_mldsa_private_key(HTML::TransferDataDecoder& decoder, JS::Realm& realm)
{
    auto seed = TRY(decoder.decode_buffer(realm));
    auto public_key = TRY(decoder.decode_buffer(realm));
    auto private_key = TRY(decoder.decode_buffer(realm));
    return ::Crypto::PK::MLDSAPrivateKey { move(seed), move(public_key), move(private_key) };
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ::Crypto::PK::MLKEMPublicKey const& key)
{
    encoder.encode(HandleTag::MLKEMPublicKey);
    encoder.encode(key.public_key());
}

WebIDL::ExceptionOr<::Crypto::PK::MLKEMPublicKey> deserialize_mlkem_public_key(HTML::TransferDataDecoder& decoder, JS::Realm& realm)
{
    return ::Crypto::PK::MLKEMPublicKey { TRY(decoder.decode_buffer(realm)) };
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ::Crypto::PK::MLKEMPrivateKey const& key)
{
    encoder.encode(HandleTag::MLKEMPrivateKey);
    encoder.encode(key.seed());
    encoder.encode(key.public_key());
    encoder.encode(key.private_key());
}

WebIDL::ExceptionOr<::Crypto::PK::MLKEMPrivateKey> deserialize_mlkem_private_key(HTML::TransferDataDecoder& decoder, JS::Realm& realm)
{
    auto seed = TRY(decoder.decode_buffer(realm));
    auto public_key = TRY(decoder.decode_buffer(realm));
    auto private_key = TRY(decoder.decode_buffer(realm));
    return ::Crypto::PK::MLKEMPrivateKey { move(seed), move(public_key), move(private_key) };
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

String CryptoKey::algorithm_name() const
{
    if (m_algorithm_name.is_empty()) {
        auto name = MUST(m_algorithm_cached->get("name"_utf16_fly_string));
        m_algorithm_name = MUST(name.to_string(vm()));
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

WebIDL::ExceptionOr<void> CryptoKey::serialization_steps(HTML::TransferDataEncoder& serialized, bool, HTML::SerializationMemory&)
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

WebIDL::ExceptionOr<void> CryptoKey::deserialization_steps(HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory&)
{
    auto& realm = this->realm();

    // 1. Initialize the [[type]] internal slot of value to serialized.[[Type]].
    m_type = serialized.decode<Bindings::KeyType>();

    // 2. Initialize the [[extractable]] internal slot of value to serialized.[[Extractable]].
    m_extractable = serialized.decode<bool>();

    // 3. Initialize the [[algorithm]] internal slot of value to the sub-deserialization of serialized.[[Algorithm]].
    m_algorithm_cached = TRY(deserialize_key_algorithm(serialized, realm));

    // 4. Initialize the [[usages]] internal slot of value to the sub-deserialization of serialized.[[Usages]].
    auto usages = serialized.decode<Vector<Bindings::KeyUsage>>();
    set_usages(move(usages));

    // 5. Initialize the [[handle]] internal slot of value to serialized.[[Handle]].
    auto tag = serialized.decode<HandleTag>();
    switch (tag) {
    case HandleTag::ByteBuffer:
        m_key_data = TRY(serialized.decode_buffer(realm));
        break;
    case HandleTag::JsonWebKey:
        m_key_data = deserialize_json_web_key(serialized);
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
    }

    return {};
}

}
