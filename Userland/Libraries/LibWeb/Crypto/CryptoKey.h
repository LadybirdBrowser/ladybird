/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/PK/RSA.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CryptoKeyPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Crypto/CryptoBindings.h>

namespace Web::Crypto {

class CryptoKey final
    : public Bindings::PlatformObject
    , public Bindings::Serializable {
    WEB_PLATFORM_OBJECT(CryptoKey, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CryptoKey);

public:
    using InternalKeyData = Variant<ByteBuffer, Bindings::JsonWebKey, ::Crypto::PK::RSAPublicKey<>, ::Crypto::PK::RSAPrivateKey<>>;

    [[nodiscard]] static GC::Ref<CryptoKey> create(JS::Realm&, InternalKeyData);
    [[nodiscard]] static GC::Ref<CryptoKey> create(JS::Realm&);

    virtual ~CryptoKey() override;

    bool extractable() const { return m_extractable; }
    Bindings::KeyType type() const { return m_type; }
    JS::Object const* algorithm() const { return m_algorithm; }
    JS::Object const* usages() const { return m_usages; }

    Vector<Bindings::KeyUsage> internal_usages() const { return m_key_usages; }

    void set_extractable(bool extractable) { m_extractable = extractable; }
    void set_type(Bindings::KeyType type) { m_type = type; }
    void set_algorithm(GC::Ref<Object> algorithm) { m_algorithm = move(algorithm); }
    void set_usages(Vector<Bindings::KeyUsage>);

    InternalKeyData const& handle() const { return m_key_data; }
    String algorithm_name() const;

    virtual StringView interface_name() const override { return "CryptoKey"sv; }
    virtual WebIDL::ExceptionOr<void> serialization_steps(HTML::SerializationRecord& record, bool for_storage, HTML::SerializationMemory&) override;
    virtual WebIDL::ExceptionOr<void> deserialization_steps(ReadonlySpan<u32> const& record, size_t& position, HTML::DeserializationMemory&) override;

private:
    CryptoKey(JS::Realm&, InternalKeyData);
    explicit CryptoKey(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    Bindings::KeyType m_type;
    bool m_extractable { false };
    GC::Ref<Object> m_algorithm;
    GC::Ref<Object> m_usages;

    Vector<Bindings::KeyUsage> m_key_usages;
    InternalKeyData m_key_data; // [[handle]]
    mutable String m_algorithm_name;
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
