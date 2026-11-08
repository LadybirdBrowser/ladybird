/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/Crypto/CryptoKeySerializationTags.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::Crypto {

enum class SerializedKeyAlgorithmType : u8 {
    Key,
    Rsa,
    RsaHashed,
    Ec,
    Aes,
    Hmac,
    Kmac,
};

struct CryptoKeyAlgorithmFields {
    u8 type { 0 };
    FlatPtr name { 0 };
    u32 modulus_length { 0 };
    ReadonlyBytes public_exponent;
    FlatPtr hash { 0 };
    FlatPtr named_curve { 0 };
    u32 length { 0 };
};

struct CryptoKeyHandleFields {
    u8 handle_tag { 0 };
    Array<ReadonlyBytes, 8> handle_buffers;
    size_t handle_buffer_count { 0 };
    bool has_ec_parameters { false };
    ReadonlySpan<u32> ec_parameters;
    bool has_ec_public_key { false };
};

extern "C" {

CryptoKey* web_rust_crypto_key_create(JSVM*, CryptoKey::InternalKeyData*);
CryptoKey::InternalKeyData const* web_rust_crypto_key_handle(CryptoKey const*);
bool web_rust_crypto_key_extractable(CryptoKey const*);
u8 web_rust_crypto_key_type(CryptoKey const*);
void web_rust_crypto_key_set_extractable(CryptoKey*, bool);
void web_rust_crypto_key_set_type(CryptoKey*, u8);
void web_rust_crypto_key_get_algorithm(CryptoKey const*, CryptoKeyAlgorithmFields*);
void web_rust_crypto_key_set_algorithm(CryptoKey*, CryptoKeyAlgorithmFields const*);
FlatPtr web_rust_crypto_key_algorithm_name(CryptoKey const*);
void web_rust_crypto_key_with_usages(CryptoKey const*, void*, void (*)(void*, ReadonlySpan<KeyUsage>));
void web_rust_crypto_key_set_usages(CryptoKey*, ReadonlySpan<KeyUsage>);

void web_crypto_key_destroy_handle(CryptoKey::InternalKeyData*);
CryptoKey::InternalKeyData* web_crypto_key_create_byte_handle(ReadonlyBytes);
CryptoKey::InternalKeyData* web_crypto_key_create_okp_handle(bool, ReadonlyBytes);
void web_crypto_key_okp_handle_bytes(CryptoKey::InternalKeyData const*, ReadonlyBytes*);
void web_crypto_key_byte_handle_bytes(CryptoKey::InternalKeyData const*, ReadonlyBytes*);
void web_crypto_key_encode_handle(CryptoKey::InternalKeyData const*, void*, void (*)(void*, CryptoKeyHandleFields const*));
JSCompletion web_crypto_key_decode_handle(void* realm, CryptoKeyHandleFields const*, CryptoKey::InternalKeyData** out_handle);
}

GC::Ref<CryptoKey> CryptoKey::create(InternalKeyData key_data)
{
    return *web_rust_crypto_key_create(JS::HostABI::vm_to_abi(JS::VM::the()), new InternalKeyData(move(key_data)));
}

void web_crypto_key_destroy_handle(CryptoKey::InternalKeyData* handle)
{
    handle->visit(
        [](ByteBuffer& data) { secure_zero(data.data(), data.size()); },
        [](OKPPublicKey& data) { secure_zero(data.bytes.data(), data.bytes.size()); },
        [](OKPPrivateKey& data) { secure_zero(data.bytes.data(), data.bytes.size()); },
        [](auto& data) { secure_zero(reinterpret_cast<u8*>(&data), sizeof(data)); });
    delete handle;
}

CryptoKey::InternalKeyData* web_crypto_key_create_byte_handle(ReadonlyBytes bytes)
{
    return new CryptoKey::InternalKeyData(MUST(ByteBuffer::copy(bytes)));
}

void web_crypto_key_byte_handle_bytes(CryptoKey::InternalKeyData const* handle, ReadonlyBytes* bytes)
{
    *bytes = handle->get<ByteBuffer>().bytes();
}

CryptoKey::InternalKeyData* web_crypto_key_create_okp_handle(bool is_private, ReadonlyBytes bytes)
{
    auto key_bytes = MUST(ByteBuffer::copy(bytes));
    if (is_private)
        return new CryptoKey::InternalKeyData(OKPPrivateKey { move(key_bytes) });
    return new CryptoKey::InternalKeyData(OKPPublicKey { move(key_bytes) });
}

void web_crypto_key_okp_handle_bytes(CryptoKey::InternalKeyData const* handle, ReadonlyBytes* bytes)
{
    if (handle->has<OKPPrivateKey>())
        *bytes = handle->get<OKPPrivateKey>().bytes;
    else
        *bytes = handle->get<OKPPublicKey>().bytes;
}

CryptoKey::InternalKeyData const& CryptoKey::handle() const
{
    return *web_rust_crypto_key_handle(this);
}

void CryptoKey::set_algorithm(InternalAlgorithmData algorithm)
{
    CryptoKeyAlgorithmFields fields;
    Utf16String hash;
    Utf16String named_curve;
    auto name = algorithm.visit([](auto const& data) { return data.name(); });
    algorithm.visit(
        [&](KeyAlgorithmData const&) {
            fields.type = to_underlying(SerializedKeyAlgorithmType::Key);
        },
        [&](RsaKeyAlgorithmData const& data) {
            fields.type = to_underlying(SerializedKeyAlgorithmType::Rsa);
            fields.modulus_length = data.modulus_length();
            fields.public_exponent = data.public_exponent().bytes();
        },
        [&](RsaHashedKeyAlgorithmData const& data) {
            fields.type = to_underlying(SerializedKeyAlgorithmType::RsaHashed);
            fields.modulus_length = data.modulus_length();
            fields.public_exponent = data.public_exponent().bytes();
            hash = data.hash();
        },
        [&](EcKeyAlgorithmData const& data) {
            fields.type = to_underlying(SerializedKeyAlgorithmType::Ec);
            named_curve = data.named_curve();
        },
        [&](AesKeyAlgorithmData const& data) {
            fields.type = to_underlying(SerializedKeyAlgorithmType::Aes);
            fields.length = data.length();
        },
        [&](HmacKeyAlgorithmData const& data) {
            fields.type = to_underlying(SerializedKeyAlgorithmType::Hmac);
            hash = data.hash();
            fields.length = data.length();
        },
        [&](KmacKeyAlgorithmData const& data) {
            fields.type = to_underlying(SerializedKeyAlgorithmType::Kmac);
            fields.length = data.length();
        });
    fields.name = move(name).into_raw();
    fields.hash = move(hash).into_raw();
    fields.named_curve = move(named_curve).into_raw();
    web_rust_crypto_key_set_algorithm(this, &fields);
}

void CryptoKey::set_usages(Vector<KeyUsage> usages)
{
    web_rust_crypto_key_set_usages(this, usages.span());
}

bool CryptoKey::extractable() const
{
    return web_rust_crypto_key_extractable(this);
}

KeyType CryptoKey::type() const
{
    return static_cast<KeyType>(web_rust_crypto_key_type(this));
}

void CryptoKey::set_extractable(bool extractable)
{
    web_rust_crypto_key_set_extractable(this, extractable);
}

void CryptoKey::set_type(KeyType type)
{
    web_rust_crypto_key_set_type(this, to_underlying(type));
}

CryptoKey::InternalAlgorithmData CryptoKey::internal_algorithm() const
{
    CryptoKeyAlgorithmFields fields;
    web_rust_crypto_key_get_algorithm(this, &fields);
    auto name = Utf16String::from_raw(fields.name);
    switch (static_cast<SerializedKeyAlgorithmType>(fields.type)) {
    case SerializedKeyAlgorithmType::Key:
        return KeyAlgorithmData { move(name) };
    case SerializedKeyAlgorithmType::Rsa:
        return RsaKeyAlgorithmData { move(name), fields.modulus_length, MUST(ByteBuffer::copy(fields.public_exponent)) };
    case SerializedKeyAlgorithmType::RsaHashed:
        return RsaHashedKeyAlgorithmData { move(name), fields.modulus_length, MUST(ByteBuffer::copy(fields.public_exponent)), Utf16String::from_raw(fields.hash) };
    case SerializedKeyAlgorithmType::Ec:
        return EcKeyAlgorithmData { move(name), Utf16String::from_raw(fields.named_curve) };
    case SerializedKeyAlgorithmType::Aes:
        return AesKeyAlgorithmData { move(name), static_cast<u16>(fields.length) };
    case SerializedKeyAlgorithmType::Hmac:
        return HmacKeyAlgorithmData { move(name), Utf16String::from_raw(fields.hash), fields.length };
    case SerializedKeyAlgorithmType::Kmac:
        return KmacKeyAlgorithmData { move(name), fields.length };
    }
    VERIFY_NOT_REACHED();
}

Vector<KeyUsage> CryptoKey::internal_usages() const
{
    Vector<KeyUsage> usages;
    web_rust_crypto_key_with_usages(this, &usages, [](void* state, ReadonlySpan<KeyUsage> values) {
        static_cast<Vector<KeyUsage>*>(state)->append(values.data(), values.size());
    });
    return usages;
}

Utf16String CryptoKey::algorithm_name() const
{
    return Utf16String::adopt_raw(web_rust_crypto_key_algorithm_name(this));
}

static_assert(to_underlying(KeyType::Public) == 0);
static_assert(to_underlying(KeyType::Private) == 1);
static_assert(to_underlying(KeyType::Secret) == 2);
static_assert(sizeof(KeyUsage) == 1);
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

void web_crypto_key_encode_handle(CryptoKey::InternalKeyData const* handle, void* state, void (*steps)(void*, CryptoKeyHandleFields const*))
{
    CryptoKeyHandleFields fields;
    // NB: The inline capacity holds every buffer that a handle exports, so that none moves while the fields borrow it.
    Vector<ByteBuffer, 8> exported;
    auto add = [&](ReadonlyBytes bytes) { fields.handle_buffers[fields.handle_buffer_count++] = bytes; };
    auto add_exported = [&](ByteBuffer buffer) {
        exported.unchecked_append(move(buffer));
        add(exported.last().bytes());
    };
    handle->visit(
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

static WebIDL::ExceptionOr<CryptoKey::InternalKeyData> handle_from_fields(JS::VM& vm, CryptoKeyHandleFields const& fields)
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

JSCompletion web_crypto_key_decode_handle(void* realm, CryptoKeyHandleFields const* fields, CryptoKey::InternalKeyData** out_handle)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto handle = handle_from_fields(js_realm.vm(), *fields);
    if (handle.is_exception())
        return Bindings::completion_to_abi(js_realm, handle.release_error());
    *out_handle = new CryptoKey::InternalKeyData(handle.release_value());
    return Bindings::completion_to_abi(js_realm, JS::js_undefined());
}

}

namespace Web::Bindings {

extern "C" JSObject* web_rust_crypto_key_wrap(JSVM*, void*, Crypto::CryptoKey*);

GC::Ref<JS::HostObject> wrap(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, GC::Ref<Crypto::CryptoKey> key)
{
    VERIFY(&wrapper_world == &host_defined_wrapper_world(preferred_realm));
    auto* wrapper = web_rust_crypto_key_wrap(JS::HostABI::vm_to_abi(preferred_realm.vm()), &preferred_realm, key.ptr());
    return as<JS::HostObject>(*JS::HostABI::object_from_abi(wrapper));
}

}
