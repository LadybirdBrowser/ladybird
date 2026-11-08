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
#include <AK/Utf16String.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibGC/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Crypto/CryptoBindings.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::Crypto {

using NamedCurve = Utf16String;
using ExportKeyResult = Variant<ByteBuffer, JsonWebKey>;

// https://wicg.github.io/webcrypto-modern-algos/#encapsulation
struct EncapsulatedBits {
    Optional<ByteBuffer> shared_key;
    Optional<ByteBuffer> ciphertext;
};

struct HashAlgorithmIdentifier {
    Utf16String name;
};

// https://w3c.github.io/webcrypto/#algorithm-overview
struct AlgorithmParams {
    AK_ALLOC_WITH_KMALLOC;

    virtual ~AlgorithmParams();
    explicit AlgorithmParams()
    {
    }

    // NOTE: this is initialized when normalizing the algorithm name as the spec requests.
    //       It must not be set in `from_value`.
    Utf16String name;
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

class AlgorithmMethods {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~AlgorithmMethods();

    virtual WebIDL::ExceptionOr<ByteBuffer> encrypt(JS::Realm&, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create("encrypt is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<ByteBuffer> decrypt(JS::Realm&, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create("decrypt is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<ByteBuffer> sign(JS::Realm&, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create("sign is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<bool> verify(JS::Realm&, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create("verify is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<ByteBuffer> derive_bits(JS::Realm&, AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>)
    {
        return WebIDL::NotSupportedError::create("deriveBits is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm&, AlgorithmParams const&, KeyFormat, CryptoKey::ImportKeyData, bool, Vector<KeyUsage> const&)
    {
        return WebIDL::NotSupportedError::create("importKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> generate_key(JS::Realm&, AlgorithmParams const&, bool, Vector<KeyUsage> const&)
    {
        return WebIDL::NotSupportedError::create("generateKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<ExportKeyResult> export_key(JS::Realm&, KeyFormat, GC::Ref<CryptoKey>)
    {
        return WebIDL::NotSupportedError::create("exportKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<Optional<u32>> get_key_length(JS::Realm&, AlgorithmParams const&)
    {
        return WebIDL::NotSupportedError::create("getKeyLength is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<u32> get_shared_key_length(AlgorithmParams const&) const
    {
        return WebIDL::NotSupportedError::create("getSharedKeyLength is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<ByteBuffer> wrap_key(JS::Realm&, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create("wrapKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<ByteBuffer> unwrap_key(JS::Realm&, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create("unwwrapKey is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<EncapsulatedBits> encapsulate(JS::Realm&, AlgorithmParams const&, GC::Ref<CryptoKey>)
    {
        return WebIDL::NotSupportedError::create("encapsulate is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<ByteBuffer> decapsulate(JS::Realm&, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&)
    {
        return WebIDL::NotSupportedError::create("decalpsulate is not supported"_utf16);
    }

    virtual WebIDL::ExceptionOr<void> validate_encrypt_parameters(AlgorithmParams const&) const { return {}; }
    virtual WebIDL::ExceptionOr<void> validate_decrypt_parameters(AlgorithmParams const&) const { return {}; }
    virtual WebIDL::ExceptionOr<void> validate_sign_parameters(AlgorithmParams const&) const { return {}; }
    virtual WebIDL::ExceptionOr<void> validate_verify_parameters(AlgorithmParams const&) const { return {}; }
    virtual WebIDL::ExceptionOr<void> validate_generate_key_parameters(AlgorithmParams const&) const { return {}; }
    virtual WebIDL::ExceptionOr<void> validate_import_key_parameters(AlgorithmParams const&) const { return {}; }
    virtual WebIDL::ExceptionOr<void> validate_derive_bits_parameters(AlgorithmParams const&, Optional<u32>) const { return {}; }

    virtual Optional<ReadonlySpan<KeyUsage>> public_key_usages() const { return {}; }

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new AlgorithmMethods); }

protected:
    AlgorithmMethods() = default;
};

class RSAOAEP : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<ByteBuffer> encrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<ByteBuffer> decrypt(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<KeyUsage> const&) override;

    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, KeyFormat, CryptoKey::ImportKeyData, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<ExportKeyResult> export_key(JS::Realm& realm, KeyFormat, GC::Ref<CryptoKey>) override;

    virtual WebIDL::ExceptionOr<void> validate_generate_key_parameters(AlgorithmParams const&) const override;
    virtual Optional<ReadonlySpan<KeyUsage>> public_key_usages() const override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new RSAOAEP); }

private:
    RSAOAEP() = default;
};

class RSAPSS : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<ByteBuffer> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<bool> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<KeyUsage> const&) override;

    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, KeyFormat, CryptoKey::ImportKeyData, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<ExportKeyResult> export_key(JS::Realm& realm, KeyFormat, GC::Ref<CryptoKey>) override;

    virtual WebIDL::ExceptionOr<void> validate_generate_key_parameters(AlgorithmParams const&) const override;
    virtual Optional<ReadonlySpan<KeyUsage>> public_key_usages() const override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new RSAPSS); }

private:
    RSAPSS() = default;
};

class RSASSAPKCS1 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<ByteBuffer> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<bool> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<KeyUsage> const&) override;

    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, KeyFormat, CryptoKey::ImportKeyData, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<ExportKeyResult> export_key(JS::Realm& realm, KeyFormat, GC::Ref<CryptoKey>) override;

    virtual WebIDL::ExceptionOr<void> validate_generate_key_parameters(AlgorithmParams const&) const override;
    virtual Optional<ReadonlySpan<KeyUsage>> public_key_usages() const override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new RSASSAPKCS1); }

private:
    RSASSAPKCS1() = default;
};

class ECDSA : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<ByteBuffer> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<bool> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, KeyFormat, CryptoKey::ImportKeyData, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<ExportKeyResult> export_key(JS::Realm& realm, KeyFormat, GC::Ref<CryptoKey>) override;

    virtual WebIDL::ExceptionOr<void> validate_generate_key_parameters(AlgorithmParams const&) const override;
    virtual WebIDL::ExceptionOr<void> validate_import_key_parameters(AlgorithmParams const&) const override;
    virtual WebIDL::ExceptionOr<void> validate_sign_parameters(AlgorithmParams const&) const override;
    virtual WebIDL::ExceptionOr<void> validate_verify_parameters(AlgorithmParams const&) const override;
    virtual Optional<ReadonlySpan<KeyUsage>> public_key_usages() const override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new ECDSA); }

private:
    ECDSA() = default;
};

class ECDH : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, KeyFormat, CryptoKey::ImportKeyData, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<ExportKeyResult> export_key(JS::Realm& realm, KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<ByteBuffer> derive_bits(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;

    virtual WebIDL::ExceptionOr<void> validate_generate_key_parameters(AlgorithmParams const&) const override;
    virtual WebIDL::ExceptionOr<void> validate_import_key_parameters(AlgorithmParams const&) const override;
    virtual WebIDL::ExceptionOr<void> validate_derive_bits_parameters(AlgorithmParams const&, Optional<u32>) const override;
    virtual Optional<ReadonlySpan<KeyUsage>> public_key_usages() const override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new ECDH); }

private:
    ECDH() = default;
};

class ED25519 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<ByteBuffer> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<bool> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, KeyFormat, CryptoKey::ImportKeyData, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<ExportKeyResult> export_key(JS::Realm& realm, KeyFormat, GC::Ref<CryptoKey>) override;

    virtual Optional<ReadonlySpan<KeyUsage>> public_key_usages() const override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new ED25519); }

private:
    ED25519() = default;
};

class ED448 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<ByteBuffer> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<bool> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;

    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, KeyFormat, CryptoKey::ImportKeyData, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<ExportKeyResult> export_key(JS::Realm& realm, KeyFormat, GC::Ref<CryptoKey>) override;

    virtual Optional<ReadonlySpan<KeyUsage>> public_key_usages() const override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new ED448); }

private:
    ED448() = default;
};

class X25519 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<ByteBuffer> derive_bits(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, KeyFormat, CryptoKey::ImportKeyData, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<ExportKeyResult> export_key(JS::Realm& realm, KeyFormat, GC::Ref<CryptoKey>) override;

    virtual WebIDL::ExceptionOr<void> validate_derive_bits_parameters(AlgorithmParams const&, Optional<u32>) const override;
    virtual Optional<ReadonlySpan<KeyUsage>> public_key_usages() const override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new X25519); }

private:
    X25519() = default;
};

class X448 : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<ByteBuffer> derive_bits(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, Optional<u32>) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, KeyFormat, CryptoKey::ImportKeyData, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<ExportKeyResult> export_key(JS::Realm& realm, KeyFormat, GC::Ref<CryptoKey>) override;

    virtual Optional<ReadonlySpan<KeyUsage>> public_key_usages() const override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new X448); }

private:
    X448() = default;
};

class MLDSA : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<ByteBuffer> sign(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<bool> verify(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, KeyFormat, CryptoKey::ImportKeyData, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<ExportKeyResult> export_key(JS::Realm& realm, KeyFormat, GC::Ref<CryptoKey>) override;

    virtual WebIDL::ExceptionOr<void> validate_sign_parameters(AlgorithmParams const&) const override;
    virtual WebIDL::ExceptionOr<void> validate_verify_parameters(AlgorithmParams const&) const override;
    virtual Optional<ReadonlySpan<KeyUsage>> public_key_usages() const override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new MLDSA); }

private:
    MLDSA() = default;
};

class MLKEM : public AlgorithmMethods {
public:
    virtual WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> generate_key(JS::Realm& realm, AlgorithmParams const&, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<GC::Ref<CryptoKey>> import_key(JS::Realm& realm, AlgorithmParams const&, KeyFormat, CryptoKey::ImportKeyData, bool, Vector<KeyUsage> const&) override;
    virtual WebIDL::ExceptionOr<ExportKeyResult> export_key(JS::Realm& realm, KeyFormat, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<EncapsulatedBits> encapsulate(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>) override;
    virtual WebIDL::ExceptionOr<ByteBuffer> decapsulate(JS::Realm& realm, AlgorithmParams const&, GC::Ref<CryptoKey>, ByteBuffer const&) override;
    virtual WebIDL::ExceptionOr<u32> get_shared_key_length(AlgorithmParams const&) const override;

    virtual Optional<ReadonlySpan<KeyUsage>> public_key_usages() const override;

    static NonnullOwnPtr<AlgorithmMethods> create() { return adopt_own(*new MLKEM); }

private:
    MLKEM() = default;
};

struct EcdhKeyDeriveParams : public AlgorithmParams {
    virtual ~EcdhKeyDeriveParams() override;

    EcdhKeyDeriveParams(CryptoKey& public_key)
        : public_key(public_key)
    {
    }

    GC::Root<CryptoKey> public_key;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

struct EcKeyImportParams : public AlgorithmParams {
    virtual ~EcKeyImportParams() override;

    EcKeyImportParams(NamedCurve named_curve)
        : named_curve(move(named_curve))
    {
    }

    NamedCurve named_curve;

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

// https://wicg.github.io/webcrypto-modern-algos/#cshake-params
// https://wicg.github.io/webcrypto-modern-algos/#dfn-AeadParams
// NOTE: The AeadParams dictionary is identical to the AesGcmParams
Vector<KeyUsage> normalized_key_usages(ReadonlySpan<KeyUsage>);

ErrorOr<Utf16String> base64_url_uint_encode(::Crypto::UnsignedBigInteger);
WebIDL::ExceptionOr<ByteBuffer> base64_url_bytes_decode(JS::Realm&, Utf16String const& base64_url_string);
WebIDL::ExceptionOr<::Crypto::UnsignedBigInteger> base64_url_uint_decode(JS::Realm&, Utf16String const& base64_url_string);

}

GC_UNTRACED_CAPTURE(Web::Crypto::EncapsulatedBits);
