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
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Crypto/CryptoBindings.h>

namespace Web::Crypto {

// The raw key octets of an Ed25519, Ed448, X25519, or X448 ("OKP") key.
struct OKPPublicKey {
    ByteBuffer bytes;
};

struct OKPPrivateKey {
    ByteBuffer bytes;
};

class WEB_API CryptoKey final
    : public Bindings::PlatformObject
    , public Bindings::Serializable {
    WEB_PLATFORM_OBJECT(CryptoKey, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CryptoKey);

public:
    using ImportKeyData = Variant<ByteBuffer, JsonWebKey>;
    using InternalKeyData = Variant<ByteBuffer, ::Crypto::PK::RSAPublicKey, ::Crypto::PK::RSAPrivateKey, ::Crypto::PK::ECPublicKey, ::Crypto::PK::ECPrivateKey, ::Crypto::PK::MLDSAPublicKey, ::Crypto::PK::MLDSAPrivateKey, ::Crypto::PK::MLKEMPublicKey, ::Crypto::PK::MLKEMPrivateKey, OKPPublicKey, OKPPrivateKey>;

    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<CryptoKey> create(JS::Realm&, InternalKeyData);
    [[nodiscard]] static GC::Ref<CryptoKey> create(JS::Realm&);

    bool extractable() const { return m_extractable; }
    Bindings::KeyType type() const { return m_type; }
    JS::Object const* algorithm() const { return m_algorithm_cached; }
    JS::Object const* usages() const { return m_usages_cached; }

    Vector<Bindings::KeyUsage> internal_usages() const { return m_usages; }

    void set_extractable(bool extractable) { m_extractable = extractable; }
    void set_type(Bindings::KeyType type) { m_type = type; }
    void set_algorithm(GC::Ref<Object> algorithm) { m_algorithm_cached = algorithm; }
    void set_usages(Vector<Bindings::KeyUsage>);

    InternalKeyData const& handle() const { return m_key_data; }
    Utf16String const& algorithm_name() const;

    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::StructuredSerializeWriter&, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(HTML::StructuredSerializeReader&, HTML::DeserializationMemory&) override;

private:
    CryptoKey(JS::Realm&, InternalKeyData);
    explicit CryptoKey(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;
    virtual void finalize() override;

    Bindings::KeyType m_type;
    bool m_extractable { false };
    GC::Ref<Object> m_algorithm_cached;
    GC::Ref<Object> m_usages_cached;

    Vector<Bindings::KeyUsage> m_usages;
    InternalKeyData m_key_data; // [[handle]]
    mutable Utf16String m_algorithm_name;
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
