/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/String.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Bindings/SubtleCryptoPrototype.h>
#include <LibWeb/Crypto/CryptoBindings.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Crypto {

using AlgorithmIdentifier = Variant<JS::Handle<JS::Object>, String>;
using NamedCurve = String;
using KeyDataType = Variant<JS::Handle<WebIDL::BufferSource>, Bindings::JsonWebKey>;

struct HashAlgorithmIdentifier : public AlgorithmIdentifier {
    using AlgorithmIdentifier::AlgorithmIdentifier;

    JS::ThrowCompletionOr<String> name(JS::VM& vm) const
    {
        auto value = visit(
            [](String const& name) -> JS::ThrowCompletionOr<String> { return name; },
            [&](JS::Handle<JS::Object> const& obj) -> JS::ThrowCompletionOr<String> {
                auto name_property = TRY(obj->get("name"));
                return name_property.to_string(vm);
            });

        return value;
    }
};

// https://w3c.github.io/webcrypto/#algorithm-overview
struct AlgorithmParams {
    virtual ~AlgorithmParams();
    explicit AlgorithmParams(String name)
        : name(move(name))
    {
    }

    String name;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#hkdf-params
struct HKDFParams : public AlgorithmParams {
    virtual ~HKDFParams() override;
    HKDFParams(String name, HashAlgorithmIdentifier hash, ByteBuffer salt, ByteBuffer info)
        : AlgorithmParams(move(name))
        , hash(move(hash))
        , salt(move(salt))
        , info(move(info))
    {
    }

    HashAlgorithmIdentifier hash;
    ByteBuffer salt;
    ByteBuffer info;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#pbkdf2-params
struct PBKDF2Params : public AlgorithmParams {
    virtual ~PBKDF2Params() override;
    PBKDF2Params(String name, ByteBuffer salt, u32 iterations, HashAlgorithmIdentifier hash)
        : AlgorithmParams(move(name))
        , salt(move(salt))
        , iterations(iterations)
        , hash(move(hash))
    {
    }

    ByteBuffer salt;
    u32 iterations;
    HashAlgorithmIdentifier hash;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-RsaKeyGenParams
struct RsaKeyGenParams : public AlgorithmParams {
    virtual ~RsaKeyGenParams() override;

    RsaKeyGenParams(String name, u32 modulus_length, ::Crypto::UnsignedBigInteger public_exponent)
        : AlgorithmParams(move(name))
        , modulus_length(modulus_length)
        , public_exponent(move(public_exponent))
    {
    }

    u32 modulus_length;
    // NOTE that the raw data is going to be in Big Endian u8[] format
    ::Crypto::UnsignedBigInteger public_exponent;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-RsaHashedKeyGenParams
struct RsaHashedKeyGenParams : public RsaKeyGenParams {
    virtual ~RsaHashedKeyGenParams() override;

    RsaHashedKeyGenParams(String name, u32 modulus_length, ::Crypto::UnsignedBigInteger public_exponent, HashAlgorithmIdentifier hash)
        : RsaKeyGenParams(move(name), modulus_length, move(public_exponent))
        , hash(move(hash))
    {
    }

    HashAlgorithmIdentifier hash;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-RsaHashedImportParams
struct RsaHashedImportParams : public AlgorithmParams {
    virtual ~RsaHashedImportParams() override;

    RsaHashedImportParams(String name, HashAlgorithmIdentifier hash)
        : AlgorithmParams(move(name))
        , hash(move(hash))
    {
    }

    HashAlgorithmIdentifier hash;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-RsaOaepParams
struct RsaOaepParams : public AlgorithmParams {
    virtual ~RsaOaepParams() override;

    RsaOaepParams(String name, ByteBuffer label)
        : AlgorithmParams(move(name))
        , label(move(label))
    {
    }

    ByteBuffer label;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-EcdsaParams
struct EcdsaParams : public AlgorithmParams {
    virtual ~EcdsaParams() override;

    EcdsaParams(String name, HashAlgorithmIdentifier hash)
        : AlgorithmParams(move(name))
        , hash(move(hash))
    {
    }

    HashAlgorithmIdentifier hash;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-EcKeyGenParams
struct EcKeyGenParams : public AlgorithmParams {
    virtual ~EcKeyGenParams() override;

    EcKeyGenParams(String name, NamedCurve named_curve)
        : AlgorithmParams(move(name))
        , named_curve(move(named_curve))
    {
    }

    NamedCurve named_curve;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::VM&, JS::Value);
};

class AlgorithmMethods {
public:
    virtual ~AlgorithmMethods();

    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> encrypt(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "encrypt is not supported"_string);
    }

    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> decrypt(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "decrypt is not supported"_string);
    }

    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> sign(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "sign is not supported"_string);
    }

    virtual WebIDL::ExceptionOr<JS::Value> verify(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, ByteBuffer const&, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "verify is not supported"_string);
    }

    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> digest(AlgorithmParams const&, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "digest is not supported"_string);
    }

    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> derive_bits(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, Optional<u32>)
    {
        return WebIDL::NotSupportedError::create(m_realm, "deriveBits is not supported"_string);
    }

    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "importKey is not supported"_string);
    }

    virtual WebIDL::ExceptionOr<Variant<JS::NonnullGCPtr<CryptoKey>, JS::NonnullGCPtr<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "generateKey is not supported"_string);
    }

    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Object>> export_key(Bindings::KeyFormat, JS::NonnullGCPtr<CryptoKey>)
    {
        return WebIDL::NotSupportedError::create(m_realm, "exportKey is not supported"_string);
    }

    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&)
    {
        return WebIDL::NotSupportedError::create(m_realm, "getKeyLength is not supported"_string);
    }

    static NonnullOwnPtr<AlgorithmMethods> create(JS::Realm& realm) { return adopt_own(*new AlgorithmMethods(realm)); }

protected:
    explicit AlgorithmMethods(JS::Realm& realm)
        : m_realm(realm)
    {
    }

    JS::NonnullGCPtr<JS::Realm> m_realm;
};

class RSAOAEP : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> encrypt(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> decrypt(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<JS::NonnullGCPtr<CryptoKey>, JS::NonnullGCPtr<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Object>> export_key(Bindings::KeyFormat, JS::NonnullGCPtr<CryptoKey>) override;

    static NonnullOwnPtr<AlgorithmMethods> create(JS::Realm& realm) { return adopt_own(*new RSAOAEP(realm)); }

private:
    explicit RSAOAEP(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class HKDF : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> derive_bits(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create(JS::Realm& realm) { return adopt_own(*new HKDF(realm)); }

private:
    explicit HKDF(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class PBKDF2 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<CryptoKey>> import_key(AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> derive_bits(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(AlgorithmParams const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create(JS::Realm& realm) { return adopt_own(*new PBKDF2(realm)); }

private:
    explicit PBKDF2(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class SHA : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> digest(AlgorithmParams const&, ByteBuffer const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create(JS::Realm& realm) { return adopt_own(*new SHA(realm)); }

private:
    explicit SHA(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class ECDSA : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> sign(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<JS::NonnullGCPtr<CryptoKey>, JS::NonnullGCPtr<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create(JS::Realm& realm) { return adopt_own(*new ECDSA(realm)); }

private:
    explicit ECDSA(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

class ED25519 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> sign(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(AlgorithmParams const&, JS::NonnullGCPtr<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<JS::NonnullGCPtr<CryptoKey>, JS::NonnullGCPtr<CryptoKeyPair>>> generate_key(AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create(JS::Realm& realm) { return adopt_own(*new ED25519(realm)); }

private:
    explicit ED25519(JS::Realm& realm)
        : AlgorithmMethods(realm)
    {
    }
};

ErrorOr<String> base64_url_uint_encode(::Crypto::UnsignedBigInteger);
WebIDL::ExceptionOr<::Crypto::UnsignedBigInteger> base64_url_uint_decode(JS::Realm&, String const& base64_url_string);

}
