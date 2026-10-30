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
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/CryptoKey.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/Crypto/CryptoKeySerializationTags.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::Crypto {

namespace {

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
        if (entry.key.ptr().ptr() == &key)
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

GC::Ref<CryptoKey> CryptoKey::create(InternalKeyData key_data)
{
    return GC::Heap::the().allocate<CryptoKey>(move(key_data));
}

CryptoKey::CryptoKey(InternalKeyData key_data)
    : m_algorithm(KeyAlgorithmData {})
    , m_key_data(move(key_data))
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

    auto cached_object = m_cached_algorithm_objects.find_if([&](auto const& entry) { return entry.wrapper.ptr() == wrapper; });
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

// The fields of a CryptoKey, which its serialization steps in Rust (Libraries/LibWeb/Bindings/Rust/src/crypto) write and
// its deserialization steps read. The strings of a key that C++ hands over are the receiver's, and its bytes borrow from
// the key, or from what is exported of it, for the call. Rust lends what it creates a key from.
struct CryptoKeyFields {
    u8 type { 0 };
    bool extractable { false };
    u8 algorithm_type { 0 };
    FlatPtr algorithm_name { 0 };
    u32 modulus_length { 0 };
    ReadonlyBytes public_exponent;
    FlatPtr hash { 0 };
    FlatPtr named_curve { 0 };
    u32 length { 0 };
    ReadonlySpan<u8> usages;
    u8 handle_tag { 0 };
    Array<ReadonlyBytes, 8> handle_buffers;
    size_t handle_buffer_count { 0 };
    bool has_ec_parameters { false };
    ReadonlySpan<u32> ec_parameters;
    bool has_ec_public_key { false };
};

extern "C" {

void web_crypto_key_with_fields(JSObject*, void* state, void (*steps)(void* state, CryptoKeyFields const*));
JSCompletion web_crypto_key_create(void* realm, CryptoKeyFields const*, JSObject** out_key);
}

// NB: The Rust side names these values by their underlying values.
static_assert(to_underlying(KeyType::Public) == to_underlying(Bindings::KeyType::Public));
static_assert(to_underlying(KeyType::Private) == to_underlying(Bindings::KeyType::Private));
static_assert(to_underlying(KeyType::Secret) == to_underlying(Bindings::KeyType::Secret));
static_assert(to_underlying(Bindings::KeyType::Secret) == 2);
static_assert(sizeof(Bindings::KeyUsage) == 1);
static_assert(to_underlying(Bindings::KeyUsage::Encrypt) == 0);
static_assert(to_underlying(Bindings::KeyUsage::Decrypt) == 1);
static_assert(to_underlying(Bindings::KeyUsage::Sign) == 2);
static_assert(to_underlying(Bindings::KeyUsage::Verify) == 3);
static_assert(to_underlying(Bindings::KeyUsage::DeriveKey) == 4);
static_assert(to_underlying(Bindings::KeyUsage::DeriveBits) == 5);
static_assert(to_underlying(Bindings::KeyUsage::WrapKey) == 6);
static_assert(to_underlying(Bindings::KeyUsage::UnwrapKey) == 7);
static_assert(to_underlying(Bindings::KeyUsage::EncapsulateKey) == 8);
static_assert(to_underlying(Bindings::KeyUsage::EncapsulateBits) == 9);
static_assert(to_underlying(Bindings::KeyUsage::DecapsulateKey) == 10);
static_assert(to_underlying(Bindings::KeyUsage::DecapsulateBits) == 11);
static_assert(to_underlying(SerializedKeyAlgorithmType::Key) == 0);
static_assert(to_underlying(SerializedKeyAlgorithmType::Kmac) == 6);
static_assert(to_underlying(HandleTag::ECPrivateKey) == 4);
static_assert(to_underlying(HandleTag::OKPPrivateKey) == 10);

static ByteBuffer export_big_integer(::Crypto::UnsignedBigInteger const& value)
{
    auto buffer = MUST(ByteBuffer::create_zeroed(value.byte_length()));
    auto written = value.export_data(buffer.bytes());
    VERIFY(written.size() == buffer.size());
    return buffer;
}

void web_crypto_key_with_fields(JSObject* object, void* state, void (*steps)(void* state, CryptoKeyFields const*))
{
    auto const& key = Bindings::impl_from<CryptoKey>(*JS::HostABI::object_from_abi(object));
    CryptoKeyFields fields;
    fields.type = to_underlying(key.type());
    fields.extractable = key.extractable();

    Utf16String algorithm_name = key.algorithm_name();
    Utf16String hash;
    Utf16String named_curve;
    key.internal_algorithm().visit(
        [&](CryptoKey::KeyAlgorithmData const&) {
            fields.algorithm_type = to_underlying(SerializedKeyAlgorithmType::Key);
        },
        [&](CryptoKey::RsaKeyAlgorithmData const& algorithm) {
            fields.algorithm_type = to_underlying(SerializedKeyAlgorithmType::Rsa);
            fields.modulus_length = algorithm.modulus_length();
            fields.public_exponent = algorithm.public_exponent().bytes();
        },
        [&](CryptoKey::RsaHashedKeyAlgorithmData const& algorithm) {
            fields.algorithm_type = to_underlying(SerializedKeyAlgorithmType::RsaHashed);
            fields.modulus_length = algorithm.modulus_length();
            fields.public_exponent = algorithm.public_exponent().bytes();
            hash = algorithm.hash();
        },
        [&](CryptoKey::EcKeyAlgorithmData const& algorithm) {
            fields.algorithm_type = to_underlying(SerializedKeyAlgorithmType::Ec);
            named_curve = algorithm.named_curve();
        },
        [&](CryptoKey::AesKeyAlgorithmData const& algorithm) {
            fields.algorithm_type = to_underlying(SerializedKeyAlgorithmType::Aes);
            fields.length = algorithm.length();
        },
        [&](CryptoKey::HmacKeyAlgorithmData const& algorithm) {
            fields.algorithm_type = to_underlying(SerializedKeyAlgorithmType::Hmac);
            hash = algorithm.hash();
            fields.length = algorithm.length();
        },
        [&](CryptoKey::KmacKeyAlgorithmData const& algorithm) {
            fields.algorithm_type = to_underlying(SerializedKeyAlgorithmType::Kmac);
            fields.length = algorithm.length();
        });
    fields.algorithm_name = move(algorithm_name).into_raw();
    fields.hash = move(hash).into_raw();
    fields.named_curve = move(named_curve).into_raw();

    auto usages = key.internal_usages();
    fields.usages = { reinterpret_cast<u8 const*>(usages.data()), usages.size() };

    // NB: The inline capacity holds every buffer that a handle exports, so that none moves while the fields borrow it.
    Vector<ByteBuffer, 8> exported;
    auto add = [&](ReadonlyBytes bytes) { fields.handle_buffers[fields.handle_buffer_count++] = bytes; };
    auto add_exported = [&](ByteBuffer buffer) {
        exported.unchecked_append(move(buffer));
        add(exported.last().bytes());
    };
    key.handle().visit(
        [&](ByteBuffer const& buffer) {
            fields.handle_tag = to_underlying(HandleTag::ByteBuffer);
            add(buffer.bytes());
        },
        [&](::Crypto::PK::RSAPublicKey const& rsa_key) {
            fields.handle_tag = to_underlying(HandleTag::RSAPublicKey);
            add_exported(export_big_integer(rsa_key.modulus()));
            add_exported(export_big_integer(rsa_key.public_exponent()));
        },
        [&](::Crypto::PK::RSAPrivateKey const& rsa_key) {
            fields.handle_tag = to_underlying(HandleTag::RSAPrivateKey);
            add_exported(export_big_integer(rsa_key.modulus()));
            add_exported(export_big_integer(rsa_key.private_exponent()));
            add_exported(export_big_integer(rsa_key.public_exponent()));
            add_exported(export_big_integer(rsa_key.prime1()));
            add_exported(export_big_integer(rsa_key.prime2()));
            add_exported(export_big_integer(rsa_key.exponent1()));
            add_exported(export_big_integer(rsa_key.exponent2()));
            add_exported(export_big_integer(rsa_key.coefficient()));
        },
        [&](::Crypto::PK::ECPublicKey const& ec_key) {
            fields.handle_tag = to_underlying(HandleTag::ECPublicKey);
            add_exported(MUST(ec_key.x_bytes()));
            add_exported(MUST(ec_key.y_bytes()));
        },
        [&](::Crypto::PK::ECPrivateKey const& ec_key) {
            fields.handle_tag = to_underlying(HandleTag::ECPrivateKey);
            add_exported(MUST(ec_key.d_bytes()));
            fields.has_ec_parameters = ec_key.parameters().has_value();
            if (fields.has_ec_parameters)
                fields.ec_parameters = ec_key.parameters()->span();
            fields.has_ec_public_key = ec_key.public_key().has_value();
            if (fields.has_ec_public_key) {
                add_exported(MUST(ec_key.public_key()->x_bytes()));
                add_exported(MUST(ec_key.public_key()->y_bytes()));
            }
        },
        [&](::Crypto::PK::MLDSAPublicKey const& mldsa_key) {
            fields.handle_tag = to_underlying(HandleTag::MLDSAPublicKey);
            add(mldsa_key.public_key().bytes());
        },
        [&](::Crypto::PK::MLDSAPrivateKey const& mldsa_key) {
            fields.handle_tag = to_underlying(HandleTag::MLDSAPrivateKey);
            add(mldsa_key.seed().bytes());
            add(mldsa_key.public_key().bytes());
            add(mldsa_key.private_key().bytes());
        },
        [&](::Crypto::PK::MLKEMPublicKey const& mlkem_key) {
            fields.handle_tag = to_underlying(HandleTag::MLKEMPublicKey);
            add_exported(mlkem_key.public_key());
        },
        [&](::Crypto::PK::MLKEMPrivateKey const& mlkem_key) {
            fields.handle_tag = to_underlying(HandleTag::MLKEMPrivateKey);
            add(mlkem_key.seed().bytes());
            add(mlkem_key.public_key().bytes());
            add(mlkem_key.private_key().bytes());
        },
        [&](OKPPublicKey const& okp_key) {
            fields.handle_tag = to_underlying(HandleTag::OKPPublicKey);
            add(okp_key.bytes.bytes());
        },
        [&](OKPPrivateKey const& okp_key) {
            fields.handle_tag = to_underlying(HandleTag::OKPPrivateKey);
            add(okp_key.bytes.bytes());
        });

    steps(state, &fields);

    for (auto& buffer : exported)
        secure_zero(buffer.data(), buffer.size());
}

static WebIDL::ExceptionOr<CryptoKey::InternalKeyData> handle_from_fields(JS::VM& vm, CryptoKeyFields const& fields)
{
    auto buffer = [&](size_t index) {
        VERIFY(index < fields.handle_buffer_count);
        return fields.handle_buffers[index];
    };
    auto copy = [&](size_t index) -> WebIDL::ExceptionOr<ByteBuffer> {
        return TRY_OR_THROW_OOM(vm, ByteBuffer::copy(buffer(index)));
    };
    auto big_integer = [&](size_t index) { return ::Crypto::UnsignedBigInteger::import_data(buffer(index)); };

    switch (static_cast<HandleTag>(fields.handle_tag)) {
    case HandleTag::ByteBuffer:
        return TRY(copy(0));
    case HandleTag::RSAPublicKey:
        return ::Crypto::PK::RSAPublicKey { big_integer(0), big_integer(1) };
    case HandleTag::RSAPrivateKey:
        return ::Crypto::PK::RSAPrivateKey { big_integer(0), big_integer(1), big_integer(2), big_integer(3), big_integer(4), big_integer(5), big_integer(6), big_integer(7) };
    case HandleTag::ECPublicKey:
        return ::Crypto::PK::ECPublicKey { big_integer(0), big_integer(1), buffer(0).size() };
    case HandleTag::ECPrivateKey: {
        Optional<::Crypto::ASN1::ObjectIdentifier> parameters;
        if (fields.has_ec_parameters) {
            ::Crypto::ASN1::ObjectIdentifier identifier;
            TRY_OR_THROW_OOM(vm, identifier.try_append(fields.ec_parameters.data(), fields.ec_parameters.size()));
            parameters = move(identifier);
        }
        Optional<::Crypto::PK::ECPublicKey> public_key;
        if (fields.has_ec_public_key)
            public_key = ::Crypto::PK::ECPublicKey { big_integer(1), big_integer(2), buffer(1).size() };
        return ::Crypto::PK::ECPrivateKey { big_integer(0), buffer(0).size(), move(parameters), move(public_key) };
    }
    case HandleTag::MLDSAPublicKey:
        return ::Crypto::PK::MLDSAPublicKey { TRY(copy(0)) };
    case HandleTag::MLDSAPrivateKey:
        return ::Crypto::PK::MLDSAPrivateKey { TRY(copy(0)), TRY(copy(1)), TRY(copy(2)) };
    case HandleTag::MLKEMPublicKey:
        return ::Crypto::PK::MLKEMPublicKey { TRY(copy(0)) };
    case HandleTag::MLKEMPrivateKey:
        return ::Crypto::PK::MLKEMPrivateKey { TRY(copy(0)), TRY(copy(1)), TRY(copy(2)) };
    case HandleTag::OKPPublicKey:
        return OKPPublicKey { TRY(copy(0)) };
    case HandleTag::OKPPrivateKey:
        return OKPPrivateKey { TRY(copy(0)) };
    }
    VERIFY_NOT_REACHED();
}

static WebIDL::ExceptionOr<CryptoKey::InternalAlgorithmData> algorithm_from_fields(JS::VM& vm, CryptoKeyFields const& fields)
{
    auto name = Utf16String::from_raw(fields.algorithm_name);
    switch (static_cast<SerializedKeyAlgorithmType>(fields.algorithm_type)) {
    case SerializedKeyAlgorithmType::Key:
        return CryptoKey::KeyAlgorithmData { move(name) };
    case SerializedKeyAlgorithmType::Rsa:
        return CryptoKey::RsaKeyAlgorithmData { move(name), fields.modulus_length, TRY_OR_THROW_OOM(vm, ByteBuffer::copy(fields.public_exponent)) };
    case SerializedKeyAlgorithmType::RsaHashed:
        return CryptoKey::RsaHashedKeyAlgorithmData { move(name), fields.modulus_length, TRY_OR_THROW_OOM(vm, ByteBuffer::copy(fields.public_exponent)), Utf16String::from_raw(fields.hash) };
    case SerializedKeyAlgorithmType::Ec:
        return CryptoKey::EcKeyAlgorithmData { move(name), Utf16String::from_raw(fields.named_curve) };
    case SerializedKeyAlgorithmType::Aes:
        return CryptoKey::AesKeyAlgorithmData { move(name), static_cast<u16>(fields.length) };
    case SerializedKeyAlgorithmType::Hmac:
        return CryptoKey::HmacKeyAlgorithmData { move(name), Utf16String::from_raw(fields.hash), fields.length };
    case SerializedKeyAlgorithmType::Kmac:
        return CryptoKey::KmacKeyAlgorithmData { move(name), fields.length };
    }
    VERIFY_NOT_REACHED();
}

JSCompletion web_crypto_key_create(void* realm, CryptoKeyFields const* fields, JSObject** out_key)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto& vm = js_realm.vm();
    auto key = [&]() -> WebIDL::ExceptionOr<GC::Ref<CryptoKey>> {
        auto key = CryptoKey::create(TRY(handle_from_fields(vm, *fields)));
        key->set_type(static_cast<KeyType>(fields->type));
        key->set_extractable(fields->extractable);
        key->set_algorithm(TRY(algorithm_from_fields(vm, *fields)));
        Vector<KeyUsage> usages;
        TRY_OR_THROW_OOM(vm, usages.try_ensure_capacity(fields->usages.size()));
        for (auto usage : fields->usages)
            usages.unchecked_append(static_cast<KeyUsage>(usage));
        key->set_usages(move(usages));
        return key;
    }();
    if (key.is_exception())
        return Bindings::completion_to_abi(js_realm, key.release_error());
    *out_key = JS::HostABI::object_to_abi(Bindings::wrap(Bindings::host_defined_wrapper_world(js_realm), js_realm, key.release_value()).ptr());
    return Bindings::completion_to_abi(js_realm, JS::js_undefined());
}

}
