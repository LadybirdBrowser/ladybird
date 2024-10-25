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
    JS_DECLARE_ALLOCATOR(KeyAlgorithm);

public:
    static JS::NonnullGCPtr<KeyAlgorithm> create(JS::Realm&);
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
    JS::NonnullGCPtr<JS::Realm> m_realm;
};

// https://w3c.github.io/webcrypto/#RsaKeyAlgorithm-dictionary
class RsaKeyAlgorithm : public KeyAlgorithm {
    JS_OBJECT(RsaKeyAlgorithm, KeyAlgorithm);
    JS_DECLARE_ALLOCATOR(RsaKeyAlgorithm);

public:
    static JS::NonnullGCPtr<RsaKeyAlgorithm> create(JS::Realm&);

    virtual ~RsaKeyAlgorithm() override = default;

    u32 modulus_length() const { return m_modulus_length; }
    void set_modulus_length(u32 modulus_length) { m_modulus_length = modulus_length; }

    JS::NonnullGCPtr<JS::Uint8Array> public_exponent() const { return m_public_exponent; }
    void set_public_exponent(JS::NonnullGCPtr<JS::Uint8Array> public_exponent) { m_public_exponent = public_exponent; }
    WebIDL::ExceptionOr<void> set_public_exponent(::Crypto::UnsignedBigInteger);

protected:
    RsaKeyAlgorithm(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

private:
    JS_DECLARE_NATIVE_FUNCTION(modulus_length_getter);
    JS_DECLARE_NATIVE_FUNCTION(public_exponent_getter);

    u32 m_modulus_length { 0 };
    JS::NonnullGCPtr<JS::Uint8Array> m_public_exponent;
};

// https://w3c.github.io/webcrypto/#RsaHashedKeyAlgorithm-dictionary
class RsaHashedKeyAlgorithm : public RsaKeyAlgorithm {
    JS_OBJECT(RsaHashedKeyAlgorithm, RsaKeyAlgorithm);
    JS_DECLARE_ALLOCATOR(RsaHashedKeyAlgorithm);

public:
    static JS::NonnullGCPtr<RsaHashedKeyAlgorithm> create(JS::Realm&);

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
    JS_DECLARE_ALLOCATOR(EcKeyAlgorithm);

public:
    static JS::NonnullGCPtr<EcKeyAlgorithm> create(JS::Realm&);

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

// https://w3c.github.io/webcrypto/#AesKeyAlgorithm-dictionary
struct AesKeyAlgorithm : public KeyAlgorithm {
    JS_OBJECT(AesKeyAlgorithm, KeyAlgorithm);
    JS_DECLARE_ALLOCATOR(AesKeyAlgorithm);

public:
    static JS::NonnullGCPtr<AesKeyAlgorithm> create(JS::Realm&);

    virtual ~AesKeyAlgorithm() override = default;

    u16 length() const { return m_length; }
    void set_length(u16 length) { m_length = length; }

protected:
    AesKeyAlgorithm(JS::Realm&);

    virtual void initialize(JS::Realm&) override;

private:
    JS_DECLARE_NATIVE_FUNCTION(length_getter);

    u16 m_length;
};

}
