/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Crypto/CryptoAlgorithms.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Crypto {

// https://w3c.github.io/webcrypto/#key-algorithm-dictionary
class KeyAlgorithm : public JS::Object {
    JS_OBJECT(KeyAlgorithm, JS::Object);
    GC_DECLARE_ALLOCATOR(KeyAlgorithm);

public:
    static GC::Ref<KeyAlgorithm> create(JS::Realm&);
    virtual ~KeyAlgorithm() override = default;

    String const& name() const { return m_name; }
    void set_name(String name) { m_name = move(name); }

    JS::Realm& realm() const { return m_realm; }

protected:
    KeyAlgorithm(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

private:
    JS_DECLARE_NATIVE_FUNCTION(name_getter);

    String m_name;
    GC::Ref<JS::Realm> m_realm;
};

// https://w3c.github.io/webcrypto/#RsaKeyAlgorithm-dictionary
class RsaKeyAlgorithm : public KeyAlgorithm {
    JS_OBJECT(RsaKeyAlgorithm, KeyAlgorithm);
    GC_DECLARE_ALLOCATOR(RsaKeyAlgorithm);

public:
    static GC::Ref<RsaKeyAlgorithm> create(JS::Realm&);

    virtual ~RsaKeyAlgorithm() override = default;

    u32 modulus_length() const { return m_modulus_length; }
    void set_modulus_length(u32 modulus_length) { m_modulus_length = modulus_length; }

    GC::Ref<JS::Uint8Array> public_exponent() const { return m_public_exponent; }
    void set_public_exponent(GC::Ref<JS::Uint8Array> public_exponent) { m_public_exponent = public_exponent; }
    WebIDL::ExceptionOr<void> set_public_exponent(::Crypto::UnsignedBigInteger);

protected:
    RsaKeyAlgorithm(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

private:
    JS_DECLARE_NATIVE_FUNCTION(modulus_length_getter);
    JS_DECLARE_NATIVE_FUNCTION(public_exponent_getter);

    u32 m_modulus_length { 0 };
    GC::Ref<JS::Uint8Array> m_public_exponent;
};

// https://w3c.github.io/webcrypto/#RsaHashedKeyAlgorithm-dictionary
class RsaHashedKeyAlgorithm : public RsaKeyAlgorithm {
    JS_OBJECT(RsaHashedKeyAlgorithm, RsaKeyAlgorithm);
    GC_DECLARE_ALLOCATOR(RsaHashedKeyAlgorithm);

public:
    static GC::Ref<RsaHashedKeyAlgorithm> create(JS::Realm&);

    virtual ~RsaHashedKeyAlgorithm() override = default;

    HashAlgorithmIdentifier const& hash() const { return m_hash; }
    void set_hash(HashAlgorithmIdentifier hash) { m_hash = move(hash); }

protected:
    RsaHashedKeyAlgorithm(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

private:
    JS_DECLARE_NATIVE_FUNCTION(hash_getter);

    HashAlgorithmIdentifier m_hash;
};

// https://w3c.github.io/webcrypto/#EcKeyAlgorithm-dictionary
class EcKeyAlgorithm : public KeyAlgorithm {
    JS_OBJECT(EcKeyAlgorithm, KeyAlgorithm);
    GC_DECLARE_ALLOCATOR(EcKeyAlgorithm);

public:
    static GC::Ref<EcKeyAlgorithm> create(JS::Realm&);

    virtual ~EcKeyAlgorithm() override = default;

    NamedCurve named_curve() const { return m_named_curve; }
    void set_named_curve(NamedCurve named_curve) { m_named_curve = named_curve; }

protected:
    EcKeyAlgorithm(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

private:
    JS_DECLARE_NATIVE_FUNCTION(named_curve_getter);

    NamedCurve m_named_curve;
};

}
