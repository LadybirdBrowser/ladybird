/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/PK/EC.h>
#include <LibCrypto/PK/MLDSA.h>
#include <LibCrypto/PK/MLKEM.h>
#include <LibCrypto/PK/RSA.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CryptoKey.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Crypto/CryptoBindings.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Bindings {

enum class KeyType : u8;

}

namespace Web::Crypto {

// The raw key octets of an Ed25519, Ed448, X25519, or X448 ("OKP") key.
struct OKPPublicKey {
    ByteBuffer bytes;
};

struct OKPPrivateKey {
    ByteBuffer bytes;
};

using KeyUsage = Bindings::KeyUsage;

enum class KeyType {
    Public,
    Private,
    Secret,
};

class WEB_API CryptoKey final
    : public Bindings::GCAllocatedWrappable
    , public Bindings::Serializable {
    WEB_WRAPPABLE(CryptoKey, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(CryptoKey);

public:
    using ImportKeyData = Variant<ByteBuffer, JsonWebKey>;
    using InternalKeyData = Variant<ByteBuffer, ::Crypto::PK::RSAPublicKey, ::Crypto::PK::RSAPrivateKey, ::Crypto::PK::ECPublicKey, ::Crypto::PK::ECPrivateKey, ::Crypto::PK::MLDSAPublicKey, ::Crypto::PK::MLDSAPrivateKey, ::Crypto::PK::MLKEMPublicKey, ::Crypto::PK::MLKEMPrivateKey, OKPPublicKey, OKPPrivateKey>;

    struct KeyAlgorithmData {
        Utf16String const& name() const { return m_name; }

        Utf16String m_name;
    };

    struct RsaKeyAlgorithmData {
        Utf16String const& name() const { return m_name; }
        u32 modulus_length() const { return m_modulus_length; }
        ByteBuffer const& public_exponent() const { return m_public_exponent; }

        Utf16String m_name;
        u32 m_modulus_length { 0 };
        ByteBuffer m_public_exponent;
    };

    struct RsaHashedKeyAlgorithmData {
        Utf16String const& name() const { return m_name; }
        u32 modulus_length() const { return m_modulus_length; }
        ByteBuffer const& public_exponent() const { return m_public_exponent; }
        Utf16String const& hash() const { return m_hash; }

        Utf16String m_name;
        u32 m_modulus_length { 0 };
        ByteBuffer m_public_exponent;
        Utf16String m_hash;
    };

    struct EcKeyAlgorithmData {
        Utf16String const& name() const { return m_name; }
        Utf16String const& named_curve() const { return m_named_curve; }

        Utf16String m_name;
        Utf16String m_named_curve;
    };

    struct AesKeyAlgorithmData {
        Utf16String const& name() const { return m_name; }
        u16 length() const { return m_length; }

        Utf16String m_name;
        u16 m_length { 0 };
    };

    struct HmacKeyAlgorithmData {
        Utf16String const& name() const { return m_name; }
        Utf16String const& hash() const { return m_hash; }
        WebIDL::UnsignedLong length() const { return m_length; }

        Utf16String m_name;
        Utf16String m_hash;
        WebIDL::UnsignedLong m_length { 0 };
    };

    struct KmacKeyAlgorithmData {
        Utf16String const& name() const { return m_name; }
        WebIDL::UnsignedLong length() const { return m_length; }

        Utf16String m_name;
        WebIDL::UnsignedLong m_length { 0 };
    };

    using InternalAlgorithmData = Variant<KeyAlgorithmData, RsaKeyAlgorithmData, RsaHashedKeyAlgorithmData, EcKeyAlgorithmData, AesKeyAlgorithmData, HmacKeyAlgorithmData, KmacKeyAlgorithmData>;

    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<CryptoKey> create(InternalKeyData);
    [[nodiscard]] static GC::Ref<CryptoKey> create();

    bool extractable() const { return m_extractable; }
    KeyType type() const { return m_type; }
    Bindings::KeyType bindings_type() const;
    JS::ThrowCompletionOr<JS::Value> algorithm(JS::Realm&);
    JS::ThrowCompletionOr<JS::Value> usages(JS::Realm&);

    InternalAlgorithmData const& internal_algorithm() const { return m_algorithm; }
    Vector<KeyUsage> internal_usages() const { return m_key_usages; }
    RsaHashedKeyAlgorithmData const& rsa_hashed_algorithm() const { return m_algorithm.get<RsaHashedKeyAlgorithmData>(); }
    EcKeyAlgorithmData const& ec_algorithm() const { return m_algorithm.get<EcKeyAlgorithmData>(); }
    HmacKeyAlgorithmData const& hmac_algorithm() const { return m_algorithm.get<HmacKeyAlgorithmData>(); }

    void set_extractable(bool extractable) { m_extractable = extractable; }
    void set_type(KeyType type) { m_type = type; }
    void set_algorithm(InternalAlgorithmData);
    void set_usages(Vector<KeyUsage>);

    InternalKeyData const& handle() const { return m_key_data; }
    Utf16String const& algorithm_name() const;

    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::StructuredSerializeWriter&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(JS::Realm&, HTML::StructuredSerializeReader&, HTML::DeserializationMemory&) override;

private:
    explicit CryptoKey(InternalKeyData);
    CryptoKey();

    virtual void finalize() override;
    virtual void visit_edges(Cell::Visitor&) override;

    KeyType m_type;
    bool m_extractable { false };
    InternalAlgorithmData m_algorithm;

    Vector<KeyUsage> m_key_usages;
    InternalKeyData m_key_data; // [[handle]]
    mutable Utf16String m_algorithm_name;
    struct CachedAlgorithmObject {
        GC::Weak<Bindings::PlatformObject> wrapper;
        GC::Ptr<JS::Object> object;
    };
    Vector<CachedAlgorithmObject> m_cached_algorithm_objects;
};

struct CryptoKeyPair {
    GC::Ref<CryptoKey> public_key;
    GC::Ref<CryptoKey> private_key;
};

}
