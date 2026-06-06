/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/String.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/SubtleCrypto.h>
#include <LibWeb/Crypto/CryptoBindings.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Crypto {

using AlgorithmIdentifier = Variant<GC::Ref<JS::Object>, String>;
using NamedCurve = String;
using KeyDataType = FlattenVariant<WebIDL::BufferSourceVariant, Variant<Bindings::JsonWebKey>>;

// https://wicg.github.io/webcrypto-modern-algos/#encapsulation
struct EncapsulatedKey {
    Optional<GC::Root<CryptoKey>> shared_key;
    Optional<ByteBuffer> ciphertext;

    JS::ThrowCompletionOr<GC::Ref<JS::Object>> to_object(JS::Realm&);
};

// https://wicg.github.io/webcrypto-modern-algos/#encapsulation
struct EncapsulatedBits {
    Optional<ByteBuffer> shared_key;
    Optional<ByteBuffer> ciphertext;

    JS::ThrowCompletionOr<GC::Ref<JS::Object>> to_object(JS::Realm&) const;
};

struct HashAlgorithmIdentifier : public AlgorithmIdentifier {
    using AlgorithmIdentifier::AlgorithmIdentifier;

    JS::ThrowCompletionOr<String> name(JS::VM& vm) const
    {
        auto value = visit(
            [](String const& name) -> JS::ThrowCompletionOr<String> { return name; },
            [&](GC::Root<JS::Object> const& obj) -> JS::ThrowCompletionOr<String> {
                auto name_property = TRY(obj->get("name"_utf16_fly_string));
                return name_property.to_string(vm);
            });

        return value;
    }
};

// https://w3c.github.io/webcrypto/#algorithm-overview
struct AlgorithmParams {
    virtual ~AlgorithmParams();
    explicit AlgorithmParams()
    {
    }

    // NOTE: this is initialized when normalizing the algorithm name as the spec requests.
    //       It must not be set in `from_value`.
    String name;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#aes-cbc
struct AesCbcParams : public AlgorithmParams {
    virtual ~AesCbcParams() override;
    AesCbcParams(ByteBuffer iv)
        : iv(move(iv))
    {
    }

    ByteBuffer iv;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-AesCtrParams
struct AesCtrParams : public AlgorithmParams {
    virtual ~AesCtrParams() override;
    AesCtrParams(ByteBuffer counter, u8 length)
        : counter(move(counter))
        , length(length)
    {
    }

    ByteBuffer counter;
    u8 length;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-AesGcmParams
struct AesGcmParams : public AlgorithmParams {
    virtual ~AesGcmParams() override;
    AesGcmParams(ByteBuffer iv, Optional<ByteBuffer> additional_data, Optional<u8> tag_length)
        : iv(move(iv))
        , additional_data(move(additional_data))
        , tag_length(tag_length)
    {
    }

    ByteBuffer iv;
    Optional<ByteBuffer> additional_data;
    Optional<u8> tag_length;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#hkdf-params
struct HKDFParams : public AlgorithmParams {
    virtual ~HKDFParams() override;
    HKDFParams(HashAlgorithmIdentifier hash, ByteBuffer salt, ByteBuffer info)
        : hash(move(hash))
        , salt(move(salt))
        , info(move(info))
    {
    }

    HashAlgorithmIdentifier hash;
    ByteBuffer salt;
    ByteBuffer info;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#pbkdf2-params
struct PBKDF2Params : public AlgorithmParams {
    virtual ~PBKDF2Params() override;
    PBKDF2Params(ByteBuffer salt, u32 iterations, HashAlgorithmIdentifier hash)
        : salt(move(salt))
        , iterations(iterations)
        , hash(move(hash))
    {
    }

    ByteBuffer salt;
    u32 iterations;
    HashAlgorithmIdentifier hash;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-RsaKeyGenParams
struct RsaKeyGenParams : public AlgorithmParams {
    virtual ~RsaKeyGenParams() override;

    RsaKeyGenParams(u32 modulus_length, ::Crypto::UnsignedBigInteger public_exponent)
        : modulus_length(modulus_length)
        , public_exponent(move(public_exponent))
    {
    }

    u32 modulus_length;
    // NOTE: The raw data is going to be in Big Endian u8[] format
    ::Crypto::UnsignedBigInteger public_exponent;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-RsaHashedKeyGenParams
struct RsaHashedKeyGenParams : public RsaKeyGenParams {
    virtual ~RsaHashedKeyGenParams() override;

    RsaHashedKeyGenParams(u32 modulus_length, ::Crypto::UnsignedBigInteger public_exponent, HashAlgorithmIdentifier hash)
        : RsaKeyGenParams(modulus_length, move(public_exponent))
        , hash(move(hash))
    {
    }

    HashAlgorithmIdentifier hash;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-RsaHashedImportParams
struct RsaHashedImportParams : public AlgorithmParams {
    virtual ~RsaHashedImportParams() override;

    RsaHashedImportParams(HashAlgorithmIdentifier hash)
        : hash(move(hash))
    {
    }

    HashAlgorithmIdentifier hash;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-RsaOaepParams
struct RsaOaepParams : public AlgorithmParams {
    virtual ~RsaOaepParams() override;

    RsaOaepParams(ByteBuffer label)
        : label(move(label))
    {
    }

    ByteBuffer label;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-RsaPssParams
struct RsaPssParams : public AlgorithmParams {
    virtual ~RsaPssParams() override;

    RsaPssParams(WebIDL::UnsignedLong salt_length)
        : salt_length(salt_length)
    {
    }

    WebIDL::UnsignedLong salt_length;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-EcdsaParams
struct EcdsaParams : public AlgorithmParams {
    virtual ~EcdsaParams() override;

    EcdsaParams(HashAlgorithmIdentifier hash)
        : hash(move(hash))
    {
    }

    HashAlgorithmIdentifier hash;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-EcKeyGenParams
struct EcKeyGenParams : public AlgorithmParams {
    virtual ~EcKeyGenParams() override;

    EcKeyGenParams(NamedCurve named_curve)
        : named_curve(move(named_curve))
    {
    }

    NamedCurve named_curve;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-AesKeyGenParams
struct AesKeyGenParams : public AlgorithmParams {
    virtual ~AesKeyGenParams() override;

    AesKeyGenParams(u16 length)
        : length(length)
    {
    }

    u16 length;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#dfn-AesDerivedKeyParams
struct AesDerivedKeyParams : public AlgorithmParams {
    virtual ~AesDerivedKeyParams() override;

    AesDerivedKeyParams(u16 length)
        : length(length)
    {
    }

    u16 length;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#hmac-importparams
struct HmacImportParams : public AlgorithmParams {
    virtual ~HmacImportParams() override;

    HmacImportParams(HashAlgorithmIdentifier hash, Optional<WebIDL::UnsignedLong> length)
        : hash(move(hash))
        , length(length)
    {
    }

    HashAlgorithmIdentifier hash;
    Optional<WebIDL::UnsignedLong> length;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://w3c.github.io/webcrypto/#hmac-keygen-params
struct HmacKeyGenParams : public AlgorithmParams {
    virtual ~HmacKeyGenParams() override;

    HmacKeyGenParams(HashAlgorithmIdentifier hash, Optional<WebIDL::UnsignedLong> length)
        : hash(move(hash))
        , length(length)
    {
    }

    HashAlgorithmIdentifier hash;
    Optional<WebIDL::UnsignedLong> length;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

class AlgorithmMethods {
public:
    virtual ~AlgorithmMethods();

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(realm, "encrypt is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(realm, "decrypt is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(realm, "sign is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<JS::Value> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(realm, "verify is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> digest(JS::Realm& realm, AlgorithmParams const&, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(realm, "digest is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>)
    {
        return WebIDL::NotSupportedError::create(realm, "deriveBits is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&)
    {
        return WebIDL::NotSupportedError::create(realm, "importKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&)
    {
        return WebIDL::NotSupportedError::create(realm, "generateKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>)
    {
        return WebIDL::NotSupportedError::create(realm, "exportKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(JS::Realm& realm, AlgorithmParams const&)
    {
        return WebIDL::NotSupportedError::create(realm, "getKeyLength is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> wrap_key(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(realm, "wrapKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> unwrap_key(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(realm, "unwwrapKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<EncapsulatedBits> encapsulate(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>)
    {
        return WebIDL::NotSupportedError::create(realm, "encapsulate is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decapsulate(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create(realm, "decalpsulate is not supported"_utf16);
    }

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new AlgorithmMethods); }

protected:
    AlgorithmMethods() = default;
};

class RSAOAEP : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new RSAOAEP); }

private:
    RSAOAEP() = default;
};

class RSAPSS : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new RSAPSS); }

private:
    RSAPSS() = default;
};

class RSASSAPKCS1 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new RSASSAPKCS1); }

private:
    RSASSAPKCS1() = default;
};

class AesCbc : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(JS::Realm& realm, AlgorithmParams const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new AesCbc); }

private:
    AesCbc() = default;
};

class AesCtr : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(JS::Realm& realm, AlgorithmParams const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new AesCtr); }

private:
    AesCtr() = default;
};

class AesGcm : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(JS::Realm& realm, AlgorithmParams const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new AesGcm); }

private:
    AesGcm() = default;
};

class AesKw : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(JS::Realm& realm, AlgorithmParams const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> wrap_key(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> unwrap_key(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new AesKw); }

private:
    AesKw() = default;
};

class HKDF : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(JS::Realm& realm, AlgorithmParams const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new HKDF); }

private:
    HKDF() = default;
};

class PBKDF2 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(JS::Realm& realm, AlgorithmParams const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new PBKDF2); }

private:
    PBKDF2() = default;
};

class SHA : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> digest(JS::Realm& realm, AlgorithmParams const&, ByteBuffer const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new SHA); }

private:
    SHA() = default;
};

class ECDSA : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new ECDSA); }

private:
    ECDSA() = default;
};

class ECDH : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new ECDH); }

private:
    ECDH() = default;
};

class ED25519 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new ED25519); }

private:
    ED25519() = default;
};

class ED448 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new ED448); }

private:
    ED448() = default;
};

class X25519 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new X25519); }

private:
    X25519() = default;
};

class X448 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new X448); }

private:
    X448() = default;
};

class HMAC : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(JS::Realm& realm, AlgorithmParams const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new HMAC); }

private:
    HMAC() = default;
};

class MLDSA : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new MLDSA); }

private:
    MLDSA() = default;
};

class MLKEM : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<EncapsulatedBits> encapsulate(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decapsulate(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new MLKEM); }

private:
    MLKEM() = default;
};

class Argon2 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> derive_bits(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(JS::Realm& realm, AlgorithmParams const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new Argon2); }

private:
    Argon2() = default;
};

class CShake : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> digest(JS::Realm& realm, AlgorithmParams const&, ByteBuffer const&) override;
    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new CShake); }

private:
    CShake() = default;
};

struct EcdhKeyDeriveParams : public AlgorithmParams {
    virtual ~EcdhKeyDeriveParams() override;

    EcdhKeyDeriveParams(CryptoKey& public_key)
        : public_key(public_key)
    {
    }

    GC::Ref<CryptoKey> public_key;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

struct EcKeyImportParams : public AlgorithmParams {
    virtual ~EcKeyImportParams() override;

    EcKeyImportParams(String named_curve)
        : named_curve(move(named_curve))
    {
    }

    String named_curve;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://wicg.github.io/webcrypto-secure-curves/#dfn-Ed448Params
struct Ed448Params : public AlgorithmParams {
    virtual ~Ed448Params() override;

    Ed448Params(Optional<ByteBuffer>& context)
        : context(context)
    {
    }

    Optional<ByteBuffer> context;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://wicg.github.io/webcrypto-modern-algos/#dfn-ContextParams
using ContextParams = Ed448Params;

// https://wicg.github.io/webcrypto-modern-algos/#argon2-params
struct Argon2Params : public AlgorithmParams {
    virtual ~Argon2Params() override;

    Argon2Params(ByteBuffer nonce, u32 parallelism, u32 memory, u32 passes, Optional<u8> version, Optional<ByteBuffer> secret_value, Optional<ByteBuffer> associated_data)
        : nonce(move(nonce))
        , parallelism(parallelism)
        , memory(memory)
        , passes(passes)
        , version(version)
        , secret_value(secret_value)
        , associated_data(associated_data)
    {
    }

    ByteBuffer nonce;
    u32 parallelism;
    u32 memory;
    u32 passes;
    Optional<u8> version;
    Optional<ByteBuffer> secret_value;
    Optional<ByteBuffer> associated_data;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://wicg.github.io/webcrypto-modern-algos/#cshake-params
struct CShakeParams : public AlgorithmParams {
    virtual ~CShakeParams() override;

    CShakeParams(u32 output_length, Optional<ByteBuffer> function_name, Optional<ByteBuffer> customization)
        : output_length(output_length)
        , function_name(move(function_name))
        , customization(move(customization))

    {
    }

    u32 output_length;
    Optional<ByteBuffer> function_name;
    Optional<ByteBuffer> customization;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://wicg.github.io/webcrypto-modern-algos/#kmac-params
struct KmacParams : public AlgorithmParams {
    virtual ~KmacParams() override;

    KmacParams(u32 output_length, Optional<ByteBuffer> customization)
        : output_length(output_length)
        , customization(move(customization))
    {
    }

    u32 output_length;
    Optional<ByteBuffer> customization;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://wicg.github.io/webcrypto-modern-algos/#kmac-keygen-params
struct KmacKeyGenParams : public AlgorithmParams {
    virtual ~KmacKeyGenParams() override;

    KmacKeyGenParams(Optional<WebIDL::UnsignedLong> length)
        : length(length)
    {
    }

    Optional<WebIDL::UnsignedLong> length;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://wicg.github.io/webcrypto-modern-algos/#kmac-import-params
struct KmacImportParams : public AlgorithmParams {
    virtual ~KmacImportParams() override;

    KmacImportParams(Optional<WebIDL::UnsignedLong> length)
        : length(length)
    {
    }

    Optional<WebIDL::UnsignedLong> length;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

class KMAC : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<JS::Value> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(JS::Realm& realm, AlgorithmParams const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new KMAC); }

private:
    KMAC() = default;
};

// https://wicg.github.io/webcrypto-modern-algos/#dfn-AeadParams
// NOTE: The AeadParams dictionary is identical to the AesGcmParams
struct AeadParams : public AlgorithmParams {
    virtual ~AeadParams() override;
    AeadParams(ByteBuffer iv, Optional<ByteBuffer> additional_data, Optional<u8> tag_length)
        : iv(move(iv))
        , additional_data(move(additional_data))
        , tag_length(tag_length)
    {
    }

    ByteBuffer iv;
    Optional<ByteBuffer> additional_data;
    Optional<u8> tag_length;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

class ChaCha20Poly1305 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(JS::Realm& realm, AlgorithmParams const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new ChaCha20Poly1305); }

private:
    ChaCha20Poly1305() = default;
};

class AesOcb : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<JS::Value> get_key_length(JS::Realm& realm, AlgorithmParams const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, Bindings::KeyFormat, CryptoKey::InternalKeyData, bool, Vector<Bindings::KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::Object>> export_key(JS::Realm& realm, Bindings::KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> encrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> decrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<Bindings::KeyUsage> const&) override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new AesOcb); }

private:
    AesOcb() = default;
};

ErrorOr<String> base64_url_uint_encode(::Crypto::UnsignedBigInteger);
WebIDL::ExceptionOr<ByteBuffer> base64_url_bytes_decode(JS::Realm&, String const& base64_url_string);
WebIDL::ExceptionOr<::Crypto::UnsignedBigInteger> base64_url_uint_decode(JS::Realm&, String const& base64_url_string);

}
