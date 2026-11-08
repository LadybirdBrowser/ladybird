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

// https://wicg.github.io/webcrypto-modern-algos/#dfn-ContextParams
struct ContextParams : public AlgorithmParams {
    virtual ~ContextParams() override;

    ContextParams(Optional<ByteBuffer>& context)
        : context(context)
    {
    }

    Optional<ByteBuffer> context;

    static JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> from_value(JS::Realm&, JS::Value);
};

// https://wicg.github.io/webcrypto-modern-algos/#cshake-params
// https://wicg.github.io/webcrypto-modern-algos/#dfn-AeadParams
// NOTE: The AeadParams dictionary is identical to the AesGcmParams
Vector<KeyUsage> normalized_key_usages(ReadonlySpan<KeyUsage>);

ErrorOr<Utf16String> base64_url_uint_encode(::Crypto::UnsignedBigInteger);
WebIDL::ExceptionOr<ByteBuffer> base64_url_bytes_decode(JS::Realm&, Utf16String const& base64_url_string);
WebIDL::ExceptionOr<::Crypto::UnsignedBigInteger> base64_url_uint_decode(JS::Realm&, Utf16String const& base64_url_string);

}

GC_UNTRACED_CAPTURE(Web::Crypto::EncapsulatedBits);
