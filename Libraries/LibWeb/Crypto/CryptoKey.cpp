/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Crypto/CryptoKey.h>
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
}

GC::Ref<CryptoKey> CryptoKey::create(InternalKeyData key_data)
{
    return *web_rust_crypto_key_create(JS::HostABI::vm_to_abi(JS::VM::the()), new InternalKeyData(move(key_data)));
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

CryptoKey::EcKeyAlgorithmData CryptoKey::ec_algorithm() const
{
    CryptoKeyAlgorithmFields fields;
    web_rust_crypto_key_get_algorithm(this, &fields);
    VERIFY(fields.type == to_underlying(SerializedKeyAlgorithmType::Ec));
    return { Utf16String::from_raw(fields.name), Utf16String::from_raw(fields.named_curve) };
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
