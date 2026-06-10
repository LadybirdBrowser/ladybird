/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/CryptoKey.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Crypto/CryptoKey.h>
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

struct CryptoKeyCacheEntry {
    GC::Weak<CryptoKey> key;
    Bindings::WrapperWorldWeakValueCache<JS::Object> algorithm_objects;
    Bindings::WrapperWorldWeakValueCache<JS::Object> usages_objects;
};

static Vector<CryptoKeyCacheEntry>& crypto_key_caches()
{
    static NeverDestroyed<Vector<CryptoKeyCacheEntry>> caches;
    return *caches;
}

static void prune_crypto_key_caches()
{
    crypto_key_caches().remove_all_matching([](auto const& entry) {
        return !entry.key;
    });
}

static CryptoKeyCacheEntry& cache_for(CryptoKey& key)
{
    auto& caches = crypto_key_caches();
    prune_crypto_key_caches();

    for (auto& entry : caches) {
        if (entry.key.ptr() == &key)
            return entry;
    }

    caches.append(CryptoKeyCacheEntry { key, {}, {} });
    return caches.last();
}

static JS::ThrowCompletionOr<GC::Ref<JS::Uint8Array>> create_uint8_array_from_bytes(JS::Realm& realm, ReadonlyBytes bytes)
{
    auto array = TRY(JS::Uint8Array::create(realm, bytes.size()));
    array->viewed_array_buffer()->overwrite(0, bytes.data(), bytes.size());
    return array;
}

static JS::ThrowCompletionOr<GC::Ref<JS::Object>> create_key_algorithm_object(JS::Realm& realm, String const& name)
{
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    TRY(object->create_data_property_or_throw("name"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), name)));
    return object;
}

static JS::ThrowCompletionOr<GC::Ref<JS::Object>> create_algorithm_object(JS::Realm& realm, CryptoKey::InternalAlgorithmData const& algorithm)
{
    return TRY(algorithm.visit(
        [&](CryptoKey::KeyAlgorithmData const& algorithm) -> JS::ThrowCompletionOr<GC::Ref<JS::Object>> {
            return create_key_algorithm_object(realm, algorithm.name());
        },
        [&](CryptoKey::RsaKeyAlgorithmData const& algorithm) -> JS::ThrowCompletionOr<GC::Ref<JS::Object>> {
            auto object = TRY(create_key_algorithm_object(realm, algorithm.name()));
            TRY(object->create_data_property_or_throw("modulusLength"_utf16_fly_string, JS::Value(algorithm.modulus_length())));
            TRY(object->create_data_property_or_throw("publicExponent"_utf16_fly_string, TRY(create_uint8_array_from_bytes(realm, algorithm.public_exponent().bytes()))));
            return object;
        },
        [&](CryptoKey::RsaHashedKeyAlgorithmData const& algorithm) -> JS::ThrowCompletionOr<GC::Ref<JS::Object>> {
            auto object = TRY(create_key_algorithm_object(realm, algorithm.name()));
            TRY(object->create_data_property_or_throw("modulusLength"_utf16_fly_string, JS::Value(algorithm.modulus_length())));
            TRY(object->create_data_property_or_throw("publicExponent"_utf16_fly_string, TRY(create_uint8_array_from_bytes(realm, algorithm.public_exponent().bytes()))));
            TRY(object->create_data_property_or_throw("hash"_utf16_fly_string, TRY(create_key_algorithm_object(realm, algorithm.hash()))));
            return object;
        },
        [&](CryptoKey::EcKeyAlgorithmData const& algorithm) -> JS::ThrowCompletionOr<GC::Ref<JS::Object>> {
            auto object = TRY(create_key_algorithm_object(realm, algorithm.name()));
            TRY(object->create_data_property_or_throw("namedCurve"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), algorithm.named_curve())));
            return object;
        },
        [&](CryptoKey::AesKeyAlgorithmData const& algorithm) -> JS::ThrowCompletionOr<GC::Ref<JS::Object>> {
            auto object = TRY(create_key_algorithm_object(realm, algorithm.name()));
            TRY(object->create_data_property_or_throw("length"_utf16_fly_string, JS::Value(algorithm.length())));
            return object;
        },
        [&](CryptoKey::HmacKeyAlgorithmData const& algorithm) -> JS::ThrowCompletionOr<GC::Ref<JS::Object>> {
            auto object = TRY(create_key_algorithm_object(realm, algorithm.name()));
            TRY(object->create_data_property_or_throw("hash"_utf16_fly_string, TRY(create_key_algorithm_object(realm, algorithm.hash()))));
            TRY(object->create_data_property_or_throw("length"_utf16_fly_string, JS::Value(algorithm.length())));
            return object;
        },
        [&](CryptoKey::KmacKeyAlgorithmData const& algorithm) -> JS::ThrowCompletionOr<GC::Ref<JS::Object>> {
            auto object = TRY(create_key_algorithm_object(realm, algorithm.name()));
            TRY(object->create_data_property_or_throw("length"_utf16_fly_string, JS::Value(algorithm.length())));
            return object;
        }));
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
    RsaOtherPrimesInfo info;
    info.r = decoder.decode<Optional<String>>();
    info.d = decoder.decode<Optional<String>>();
    info.t = decoder.decode<Optional<String>>();
    return info;
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

WebIDL::ExceptionOr<::Crypto::PK::RSAPublicKey> deserialize_rsa_public_key(HTML::TransferDataDecoder& decoder)
{
    auto modulus = TRY(decoder.decode_unsigned_big_integer());
    auto public_exponent = TRY(decoder.decode_unsigned_big_integer());
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

WebIDL::ExceptionOr<::Crypto::PK::RSAPrivateKey> deserialize_rsa_private_key(HTML::TransferDataDecoder& decoder)
{
    auto modulus = TRY(decoder.decode_unsigned_big_integer());
    auto private_exponent = TRY(decoder.decode_unsigned_big_integer());
    auto public_exponent = TRY(decoder.decode_unsigned_big_integer());
    auto prime1 = TRY(decoder.decode_unsigned_big_integer());
    auto prime2 = TRY(decoder.decode_unsigned_big_integer());
    auto exponent1 = TRY(decoder.decode_unsigned_big_integer());
    auto exponent2 = TRY(decoder.decode_unsigned_big_integer());
    auto coefficient = TRY(decoder.decode_unsigned_big_integer());
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

WebIDL::ExceptionOr<::Crypto::PK::ECPublicKey> deserialize_ec_public_key(HTML::TransferDataDecoder& decoder)
{
    auto x_bytes = TRY(decoder.decode_buffer());
    auto y_bytes = TRY(decoder.decode_buffer());
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

WebIDL::ExceptionOr<::Crypto::PK::ECPrivateKey> deserialize_ec_private_key(HTML::TransferDataDecoder& decoder)
{
    auto d_bytes = TRY(decoder.decode_buffer());
    auto scalar_size = d_bytes.size();
    auto d = ::Crypto::UnsignedBigInteger::import_data(d_bytes);
    auto parameters = decoder.decode<Optional<Vector<int>>>();

    Optional<::Crypto::PK::ECPublicKey> public_key;
    if (decoder.decode<bool>())
        public_key = TRY(deserialize_ec_public_key(decoder));

    return ::Crypto::PK::ECPrivateKey { move(d), scalar_size, move(parameters), move(public_key) };
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ::Crypto::PK::MLDSAPublicKey const& key)
{
    encoder.encode(HandleTag::MLDSAPublicKey);
    encoder.encode(key.public_key());
}

WebIDL::ExceptionOr<::Crypto::PK::MLDSAPublicKey> deserialize_mldsa_public_key(HTML::TransferDataDecoder& decoder)
{
    return ::Crypto::PK::MLDSAPublicKey { TRY(decoder.decode_buffer()) };
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ::Crypto::PK::MLDSAPrivateKey const& key)
{
    encoder.encode(HandleTag::MLDSAPrivateKey);
    encoder.encode(key.seed());
    encoder.encode(key.public_key());
    encoder.encode(key.private_key());
}

WebIDL::ExceptionOr<::Crypto::PK::MLDSAPrivateKey> deserialize_mldsa_private_key(HTML::TransferDataDecoder& decoder)
{
    auto seed = TRY(decoder.decode_buffer());
    auto public_key = TRY(decoder.decode_buffer());
    auto private_key = TRY(decoder.decode_buffer());
    return ::Crypto::PK::MLDSAPrivateKey { move(seed), move(public_key), move(private_key) };
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ::Crypto::PK::MLKEMPublicKey const& key)
{
    encoder.encode(HandleTag::MLKEMPublicKey);
    encoder.encode(key.public_key());
}

WebIDL::ExceptionOr<::Crypto::PK::MLKEMPublicKey> deserialize_mlkem_public_key(HTML::TransferDataDecoder& decoder)
{
    return ::Crypto::PK::MLKEMPublicKey { TRY(decoder.decode_buffer()) };
}

void serialize_handle(HTML::TransferDataEncoder& encoder, ::Crypto::PK::MLKEMPrivateKey const& key)
{
    encoder.encode(HandleTag::MLKEMPrivateKey);
    encoder.encode(key.seed());
    encoder.encode(key.public_key());
    encoder.encode(key.private_key());
}

WebIDL::ExceptionOr<::Crypto::PK::MLKEMPrivateKey> deserialize_mlkem_private_key(HTML::TransferDataDecoder& decoder)
{
    auto seed = TRY(decoder.decode_buffer());
    auto public_key = TRY(decoder.decode_buffer());
    auto private_key = TRY(decoder.decode_buffer());
    return ::Crypto::PK::MLKEMPrivateKey { move(seed), move(public_key), move(private_key) };
}

}

GC_DEFINE_ALLOCATOR(CryptoKey);
enum class SerializedKeyAlgorithmType : u8 {
    Key,
    Rsa,
    RsaHashed,
    Ec,
    Aes,
    Hmac,
    Kmac,
};

static void serialize_algorithm(HTML::TransferDataEncoder& serialized, CryptoKey::InternalAlgorithmData const& algorithm)
{
    algorithm.visit(
        [&](CryptoKey::KeyAlgorithmData const& algorithm) {
            serialized.encode(SerializedKeyAlgorithmType::Key);
            serialized.encode(algorithm.name());
        },
        [&](CryptoKey::RsaKeyAlgorithmData const& algorithm) {
            serialized.encode(SerializedKeyAlgorithmType::Rsa);
            serialized.encode(algorithm.name());
            serialized.encode(algorithm.modulus_length());
            serialized.encode(algorithm.public_exponent());
        },
        [&](CryptoKey::RsaHashedKeyAlgorithmData const& algorithm) {
            serialized.encode(SerializedKeyAlgorithmType::RsaHashed);
            serialized.encode(algorithm.name());
            serialized.encode(algorithm.modulus_length());
            serialized.encode(algorithm.public_exponent());
            serialized.encode(algorithm.hash());
        },
        [&](CryptoKey::EcKeyAlgorithmData const& algorithm) {
            serialized.encode(SerializedKeyAlgorithmType::Ec);
            serialized.encode(algorithm.name());
            serialized.encode(algorithm.named_curve());
        },
        [&](CryptoKey::AesKeyAlgorithmData const& algorithm) {
            serialized.encode(SerializedKeyAlgorithmType::Aes);
            serialized.encode(algorithm.name());
            serialized.encode(algorithm.length());
        },
        [&](CryptoKey::HmacKeyAlgorithmData const& algorithm) {
            serialized.encode(SerializedKeyAlgorithmType::Hmac);
            serialized.encode(algorithm.name());
            serialized.encode(algorithm.hash());
            serialized.encode(algorithm.length());
        },
        [&](CryptoKey::KmacKeyAlgorithmData const& algorithm) {
            serialized.encode(SerializedKeyAlgorithmType::Kmac);
            serialized.encode(algorithm.name());
            serialized.encode(algorithm.length());
        });
}

static WebIDL::ExceptionOr<CryptoKey::InternalAlgorithmData> deserialize_algorithm(HTML::TransferDataDecoder& serialized)
{
    auto type = serialized.decode<SerializedKeyAlgorithmType>();
    switch (type) {
    case SerializedKeyAlgorithmType::Key:
        return CryptoKey::KeyAlgorithmData { serialized.decode<String>() };
    case SerializedKeyAlgorithmType::Rsa:
        return CryptoKey::RsaKeyAlgorithmData {
            serialized.decode<String>(),
            serialized.decode<u32>(),
            TRY(serialized.decode_buffer()),
        };
    case SerializedKeyAlgorithmType::RsaHashed:
        return CryptoKey::RsaHashedKeyAlgorithmData {
            serialized.decode<String>(),
            serialized.decode<u32>(),
            TRY(serialized.decode_buffer()),
            serialized.decode<String>(),
        };
    case SerializedKeyAlgorithmType::Ec:
        return CryptoKey::EcKeyAlgorithmData {
            serialized.decode<String>(),
            serialized.decode<String>(),
        };
    case SerializedKeyAlgorithmType::Aes:
        return CryptoKey::AesKeyAlgorithmData {
            serialized.decode<String>(),
            serialized.decode<u16>(),
        };
    case SerializedKeyAlgorithmType::Hmac:
        return CryptoKey::HmacKeyAlgorithmData {
            serialized.decode<String>(),
            serialized.decode<String>(),
            serialized.decode<WebIDL::UnsignedLong>(),
        };
    case SerializedKeyAlgorithmType::Kmac:
        return CryptoKey::KmacKeyAlgorithmData {
            serialized.decode<String>(),
            serialized.decode<WebIDL::UnsignedLong>(),
        };
    }
    VERIFY_NOT_REACHED();
}

GC::Ref<CryptoKey> CryptoKey::create(InternalKeyData key_data)
{
    return GC::Heap::the().allocate<CryptoKey>(move(key_data));
}

GC::Ref<CryptoKey> CryptoKey::create()
{
    return GC::Heap::the().allocate<CryptoKey>();
}

CryptoKey::CryptoKey(InternalKeyData key_data)
    : m_algorithm(KeyAlgorithmData {})
    , m_key_data(move(key_data))
{
}

CryptoKey::CryptoKey()
    : m_algorithm(KeyAlgorithmData {})
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

void CryptoKey::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

void CryptoKey::set_algorithm(InternalAlgorithmData algorithm)
{
    m_algorithm = move(algorithm);
}

void CryptoKey::set_usages(Vector<KeyUsage> usages)
{
    m_key_usages = move(usages);
}

Bindings::KeyType CryptoKey::bindings_type() const
{
    switch (m_type) {
    case KeyType::Public:
        return Bindings::KeyType::Public;
    case KeyType::Private:
        return Bindings::KeyType::Private;
    case KeyType::Secret:
        return Bindings::KeyType::Secret;
    }
    VERIFY_NOT_REACHED();
}

JS::ThrowCompletionOr<JS::Value> CryptoKey::algorithm(JS::Realm& realm)
{
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
    auto& cache = cache_for(*this);

    if (auto object = cache.algorithm_objects.get(wrapper_world))
        return object;

    auto object = TRY(create_algorithm_object(realm, m_algorithm));
    cache.algorithm_objects.set(wrapper_world, object);
    return object;
}

JS::ThrowCompletionOr<JS::Value> CryptoKey::usages(JS::Realm& realm)
{
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
    auto& cache = cache_for(*this);

    if (auto object = cache.usages_objects.get(wrapper_world))
        return object;

    auto usages = GC::make_root(JS::Array::create_from<KeyUsage>(realm, m_key_usages.span(), [&](auto& key_usage) -> JS::Value {
        return JS::PrimitiveString::create(realm.vm(), Bindings::idl_enum_to_string(key_usage));
    }));
    MUST(usages->set_integrity_level(JS::Object::IntegrityLevel::Frozen));

    cache.usages_objects.set(wrapper_world, *usages);
    return GC::Ref<JS::Object> { *usages };
}

String CryptoKey::algorithm_name() const
{
    return m_algorithm.visit([](auto const& algorithm) { return algorithm.name(); });
}

WebIDL::ExceptionOr<void> CryptoKey::serialization_steps(JS::Realm&, HTML::TransferDataEncoder& serialized, bool, HTML::SerializationMemory&)
{
    // 1. Set serialized.[[Type]] to the [[type]] internal slot of value.
    serialized.encode(m_type);

    // 2. Set serialized.[[Extractable]] to the [[extractable]] internal slot of value.
    serialized.encode(m_extractable);

    // 3. Set serialized.[[Algorithm]] to the sub-serialization of the [[algorithm]] internal slot of value.
    serialize_algorithm(serialized, m_algorithm);

    // 4. Set serialized.[[Usages]] to the sub-serialization of the [[usages]] internal slot of value.
    serialized.encode(m_key_usages);

    // 5. Set serialized.[[Handle]] to the [[handle]] internal slot of value.
    m_key_data.visit([&](auto const& handle) { serialize_handle(serialized, handle); });

    return {};
}

WebIDL::ExceptionOr<void> CryptoKey::deserialization_steps(JS::Realm&, HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory&)
{
    // 1. Initialize the [[type]] internal slot of value to serialized.[[Type]].
    m_type = serialized.decode<KeyType>();

    // 2. Initialize the [[extractable]] internal slot of value to serialized.[[Extractable]].
    m_extractable = serialized.decode<bool>();

    // 3. Initialize the [[algorithm]] internal slot of value to the sub-deserialization of serialized.[[Algorithm]].
    m_algorithm = TRY(deserialize_algorithm(serialized));

    // 4. Initialize the [[usages]] internal slot of value to the sub-deserialization of serialized.[[Usages]].
    m_key_usages = serialized.decode<Vector<KeyUsage>>();

    // 5. Initialize the [[handle]] internal slot of value to serialized.[[Handle]].
    auto tag = serialized.decode<HandleTag>();
    switch (tag) {
    case HandleTag::ByteBuffer:
        m_key_data = TRY(serialized.decode_buffer());
        break;
    case HandleTag::JsonWebKey:
        m_key_data = deserialize_json_web_key(serialized);
        break;
    case HandleTag::RSAPublicKey:
        m_key_data = TRY(deserialize_rsa_public_key(serialized));
        break;
    case HandleTag::RSAPrivateKey:
        m_key_data = TRY(deserialize_rsa_private_key(serialized));
        break;
    case HandleTag::ECPublicKey:
        m_key_data = TRY(deserialize_ec_public_key(serialized));
        break;
    case HandleTag::ECPrivateKey:
        m_key_data = TRY(deserialize_ec_private_key(serialized));
        break;
    case HandleTag::MLDSAPublicKey:
        m_key_data = TRY(deserialize_mldsa_public_key(serialized));
        break;
    case HandleTag::MLDSAPrivateKey:
        m_key_data = TRY(deserialize_mldsa_private_key(serialized));
        break;
    case HandleTag::MLKEMPublicKey:
        m_key_data = TRY(deserialize_mlkem_public_key(serialized));
        break;
    case HandleTag::MLKEMPrivateKey:
        m_key_data = TRY(deserialize_mlkem_private_key(serialized));
        break;
    }

    return {};
}

}
