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
#include <LibWeb/Crypto/CryptoKeySerializationTags.h>
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

struct CryptoKeyCacheEntry {
    GC::Weak<CryptoKey> key;
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

    caches.append(CryptoKeyCacheEntry { key, {} });
    return caches.last();
}

static JS::ThrowCompletionOr<GC::Ref<JS::Uint8Array>> create_uint8_array_from_bytes(JS::Realm& realm, ReadonlyBytes bytes)
{
    auto array = TRY(JS::Uint8Array::create(realm, bytes.size()));
    array->viewed_array_buffer()->overwrite(0, bytes.data(), bytes.size());
    return array;
}

static JS::ThrowCompletionOr<GC::Ref<JS::Object>> create_key_algorithm_object(JS::Realm& realm, Utf16String const& name)
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
enum class SerializedKeyAlgorithmType : u8 {
    Key,
    Rsa,
    RsaHashed,
    Ec,
    Aes,
    Hmac,
    Kmac,
};

static void serialize_algorithm(HTML::StructuredSerializeWriter& serialized, CryptoKey::InternalAlgorithmData const& algorithm)
{
    algorithm.visit(
        [&](CryptoKey::KeyAlgorithmData const& algorithm) {
            serialized.encode(to_underlying(SerializedKeyAlgorithmType::Key));
            serialized.encode(algorithm.name());
        },
        [&](CryptoKey::RsaKeyAlgorithmData const& algorithm) {
            serialized.encode(to_underlying(SerializedKeyAlgorithmType::Rsa));
            serialized.encode(algorithm.name());
            serialized.encode(algorithm.modulus_length());
            serialized.encode(algorithm.public_exponent());
        },
        [&](CryptoKey::RsaHashedKeyAlgorithmData const& algorithm) {
            serialized.encode(to_underlying(SerializedKeyAlgorithmType::RsaHashed));
            serialized.encode(algorithm.name());
            serialized.encode(algorithm.modulus_length());
            serialized.encode(algorithm.public_exponent());
            serialized.encode(algorithm.hash());
        },
        [&](CryptoKey::EcKeyAlgorithmData const& algorithm) {
            serialized.encode(to_underlying(SerializedKeyAlgorithmType::Ec));
            serialized.encode(algorithm.name());
            serialized.encode(algorithm.named_curve());
        },
        [&](CryptoKey::AesKeyAlgorithmData const& algorithm) {
            serialized.encode(to_underlying(SerializedKeyAlgorithmType::Aes));
            serialized.encode(algorithm.name());
            serialized.encode(algorithm.length());
        },
        [&](CryptoKey::HmacKeyAlgorithmData const& algorithm) {
            serialized.encode(to_underlying(SerializedKeyAlgorithmType::Hmac));
            serialized.encode(algorithm.name());
            serialized.encode(algorithm.hash());
            serialized.encode(algorithm.length());
        },
        [&](CryptoKey::KmacKeyAlgorithmData const& algorithm) {
            serialized.encode(to_underlying(SerializedKeyAlgorithmType::Kmac));
            serialized.encode(algorithm.name());
            serialized.encode(algorithm.length());
        });
}

static WebIDL::ExceptionOr<CryptoKey::InternalAlgorithmData> deserialize_algorithm(HTML::StructuredSerializeReader& serialized, JS::Realm& realm)
{
    auto type = static_cast<SerializedKeyAlgorithmType>(TRY(HTML::decode_or_throw_data_clone_error<u8>(realm, serialized)));
    switch (type) {
    case SerializedKeyAlgorithmType::Key:
        return CryptoKey::KeyAlgorithmData { TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, serialized)) };
    case SerializedKeyAlgorithmType::Rsa:
        return CryptoKey::RsaKeyAlgorithmData {
            TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, serialized)),
            TRY(HTML::decode_or_throw_data_clone_error<u32>(realm, serialized)),
            TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, serialized)),
        };
    case SerializedKeyAlgorithmType::RsaHashed:
        return CryptoKey::RsaHashedKeyAlgorithmData {
            TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, serialized)),
            TRY(HTML::decode_or_throw_data_clone_error<u32>(realm, serialized)),
            TRY(HTML::decode_or_throw_data_clone_error<ByteBuffer>(realm, serialized)),
            TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, serialized)),
        };
    case SerializedKeyAlgorithmType::Ec:
        return CryptoKey::EcKeyAlgorithmData {
            TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, serialized)),
            TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, serialized)),
        };
    case SerializedKeyAlgorithmType::Aes:
        return CryptoKey::AesKeyAlgorithmData {
            TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, serialized)),
            TRY(HTML::decode_or_throw_data_clone_error<u16>(realm, serialized)),
        };
    case SerializedKeyAlgorithmType::Hmac:
        return CryptoKey::HmacKeyAlgorithmData {
            TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, serialized)),
            TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, serialized)),
            TRY(HTML::decode_or_throw_data_clone_error<WebIDL::UnsignedLong>(realm, serialized)),
        };
    case SerializedKeyAlgorithmType::Kmac:
        return CryptoKey::KmacKeyAlgorithmData {
            TRY(HTML::decode_or_throw_data_clone_error<Utf16String>(realm, serialized)),
            TRY(HTML::decode_or_throw_data_clone_error<WebIDL::UnsignedLong>(realm, serialized)),
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
        [](OKPPublicKey& data) { secure_zero(data.bytes.data(), data.bytes.size()); },
        [](OKPPrivateKey& data) { secure_zero(data.bytes.data(), data.bytes.size()); },
        [](auto& data) { secure_zero(reinterpret_cast<u8*>(&data), sizeof(data)); });
}

void CryptoKey::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& cached_algorithm_object : m_cached_algorithm_objects)
        visitor.visit(cached_algorithm_object.object);
}

void CryptoKey::set_algorithm(InternalAlgorithmData algorithm)
{
    m_algorithm = move(algorithm);
    m_algorithm_name = {};
    m_cached_algorithm_objects.clear();
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
    auto wrapper = Bindings::wrap(wrapper_world, realm, GC::Ref { *this });
    m_cached_algorithm_objects.remove_all_matching([](auto const& entry) { return !entry.wrapper; });

    auto cached_object = m_cached_algorithm_objects.find_if([&](auto const& entry) { return entry.wrapper.ptr() == wrapper.ptr(); });
    if (!cached_object.is_end())
        return cached_object->object;

    auto object = TRY(create_algorithm_object(realm, m_algorithm));
    m_cached_algorithm_objects.append({ wrapper, object });
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

Utf16String const& CryptoKey::algorithm_name() const
{
    if (m_algorithm_name.is_empty())
        m_algorithm_name = m_algorithm.visit([](auto const& algorithm) { return algorithm.name(); });
    return m_algorithm_name;
}

WebIDL::ExceptionOr<void> CryptoKey::serialization_steps(HTML::StructuredSerializeWriter& serialized, bool, HTML::SerializationMemory&)
{
    // 1. Set serialized.[[Type]] to the [[type]] internal slot of value.
    if (serialized.type() == HTML::SerializationType::Storage)
        serialized.encode(bindings_type());
    else
        serialized.encode(to_underlying(m_type));

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

WebIDL::ExceptionOr<void> CryptoKey::deserialization_steps(JS::Realm& realm, HTML::StructuredSerializeReader& serialized, HTML::DeserializationMemory&)
{
    // 1. Initialize the [[type]] internal slot of value to serialized.[[Type]].
    if (serialized.type() == HTML::SerializationType::Storage) {
        auto type = TRY(HTML::decode_or_throw_data_clone_error<Bindings::KeyType>(realm, serialized));
        m_type = type == Bindings::KeyType::Public ? KeyType::Public : (type == Bindings::KeyType::Private ? KeyType::Private : KeyType::Secret);
    } else {
        m_type = static_cast<KeyType>(TRY(HTML::decode_or_throw_data_clone_error<int>(realm, serialized)));
    }

    // 2. Initialize the [[extractable]] internal slot of value to serialized.[[Extractable]].
    m_extractable = TRY(HTML::decode_or_throw_data_clone_error<bool>(realm, serialized));

    // 3. Initialize the [[algorithm]] internal slot of value to the sub-deserialization of serialized.[[Algorithm]].
    m_algorithm = TRY(deserialize_algorithm(serialized, realm));

    // 4. Initialize the [[usages]] internal slot of value to the sub-deserialization of serialized.[[Usages]].
    m_key_usages = TRY(HTML::decode_or_throw_data_clone_error<Vector<KeyUsage>>(realm, serialized));

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
