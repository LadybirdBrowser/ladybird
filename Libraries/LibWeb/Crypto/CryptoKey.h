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
#include <LibGC/Weak.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CryptoKey.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Crypto/CryptoBindings.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Crypto {

class CryptoKey final
    : public Bindings::Wrappable
    , public Bindings::Serializable {
    WEB_WRAPPABLE(CryptoKey, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CryptoKey);

public:
    using InternalKeyData = Variant<ByteBuffer, JsonWebKey, ::Crypto::PK::RSAPublicKey, ::Crypto::PK::RSAPrivateKey, ::Crypto::PK::ECPublicKey, ::Crypto::PK::ECPrivateKey, ::Crypto::PK::MLDSAPublicKey, ::Crypto::PK::MLDSAPrivateKey, ::Crypto::PK::MLKEMPublicKey, ::Crypto::PK::MLKEMPrivateKey>;

    struct KeyAlgorithmData {
        String const& name() const { return m_name; }

        String m_name;
    };

    struct RsaKeyAlgorithmData {
        String const& name() const { return m_name; }
        u32 modulus_length() const { return m_modulus_length; }
        ByteBuffer const& public_exponent() const { return m_public_exponent; }

        String m_name;
        u32 m_modulus_length { 0 };
        ByteBuffer m_public_exponent;
    };

    struct RsaHashedKeyAlgorithmData {
        String const& name() const { return m_name; }
        u32 modulus_length() const { return m_modulus_length; }
        ByteBuffer const& public_exponent() const { return m_public_exponent; }
        String const& hash() const { return m_hash; }

        String m_name;
        u32 m_modulus_length { 0 };
        ByteBuffer m_public_exponent;
        String m_hash;
    };

    struct EcKeyAlgorithmData {
        String const& name() const { return m_name; }
        String const& named_curve() const { return m_named_curve; }

        String m_name;
        String m_named_curve;
    };

    struct AesKeyAlgorithmData {
        String const& name() const { return m_name; }
        u16 length() const { return m_length; }

        String m_name;
        u16 m_length { 0 };
    };

    struct HmacKeyAlgorithmData {
        String const& name() const { return m_name; }
        String const& hash() const { return m_hash; }
        WebIDL::UnsignedLong length() const { return m_length; }

        String m_name;
        String m_hash;
        WebIDL::UnsignedLong m_length { 0 };
    };

    struct KmacKeyAlgorithmData {
        String const& name() const { return m_name; }
        WebIDL::UnsignedLong length() const { return m_length; }

        String m_name;
        WebIDL::UnsignedLong m_length { 0 };
    };

    using InternalAlgorithmData = Variant<KeyAlgorithmData, RsaKeyAlgorithmData, RsaHashedKeyAlgorithmData, EcKeyAlgorithmData, AesKeyAlgorithmData, HmacKeyAlgorithmData, KmacKeyAlgorithmData>;

    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<CryptoKey> create(JS::Realm&, InternalKeyData);
    [[nodiscard]] static GC::Ref<CryptoKey> create(JS::Realm&);

    bool extractable() const { return m_extractable; }
    Bindings::KeyType type() const { return m_type; }
    JS::ThrowCompletionOr<JS::Object const*> algorithm() const;
    JS::Object const* usages() const;

    Vector<Bindings::KeyUsage> internal_usages() const { return m_key_usages; }
    RsaHashedKeyAlgorithmData const& rsa_hashed_algorithm() const { return m_algorithm.get<RsaHashedKeyAlgorithmData>(); }
    EcKeyAlgorithmData const& ec_algorithm() const { return m_algorithm.get<EcKeyAlgorithmData>(); }
    HmacKeyAlgorithmData const& hmac_algorithm() const { return m_algorithm.get<HmacKeyAlgorithmData>(); }

    void set_extractable(bool extractable) { m_extractable = extractable; }
    void set_type(Bindings::KeyType type) { m_type = type; }
    void set_algorithm(GC::Ref<JS::Object>);
    void set_usages(Vector<Bindings::KeyUsage>);

    InternalKeyData const& handle() const { return m_key_data; }
    String algorithm_name() const;

    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::TransferDataEncoder&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(HTML::TransferDataDecoder&, HTML::DeserializationMemory&) override;

private:
    CryptoKey(JS::Realm&, InternalKeyData);
    explicit CryptoKey(JS::Realm&);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void finalize() override;

    JS::ThrowCompletionOr<GC::Ref<JS::Object>> algorithm_for_realm(JS::Realm&) const;
    GC::Ref<JS::Object> usages_for_realm(JS::Realm&) const;
    void clear_cached_algorithm_objects() const;
    void clear_cached_usages_objects() const;

    Bindings::KeyType m_type;
    bool m_extractable { false };
    InternalAlgorithmData m_algorithm;

    Vector<Bindings::KeyUsage> m_key_usages;
    InternalKeyData m_key_data; // [[handle]]

    mutable GC::Weak<JS::Object> m_algorithm_object;
    mutable Vector<GC::Weak<JS::Object>> m_live_algorithm_objects;
    mutable GC::Weak<JS::Object> m_usages_object;
    mutable Vector<GC::Weak<JS::Object>> m_live_usages_objects;
};

// https://w3c.github.io/webcrypto/#ref-for-dfn-CryptoKeyPair-2
class CryptoKeyPair : public JS::Object {
    JS_OBJECT(CryptoKeyPair, JS::Object);
    GC_DECLARE_ALLOCATOR(CryptoKeyPair);

public:
    static GC::Ref<CryptoKeyPair> create(JS::Realm&, GC::Ref<CryptoKey> public_key, GC::Ref<CryptoKey> private_key);
    virtual ~CryptoKeyPair() override = default;

    GC::Ref<CryptoKey> public_key() const { return m_public_key; }
    GC::Ref<CryptoKey> private_key() const { return m_private_key; }

private:
    CryptoKeyPair(JS::Realm&, GC::Ref<CryptoKey> public_key, GC::Ref<CryptoKey> private_key);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    JS_DECLARE_NATIVE_FUNCTION(public_key_getter);
    JS_DECLARE_NATIVE_FUNCTION(private_key_getter);

    GC::Ref<CryptoKey> m_public_key;
    GC::Ref<CryptoKey> m_private_key;
};

}
