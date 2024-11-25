/*
 * Copyright (c) 2020-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibCore/ConfigFile.h>
#include <LibCrypto/ASN1/Constants.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibCrypto/PK/EC.h>
#include <LibCrypto/PK/RSA.h>

namespace Crypto::Certificate {

struct AlgorithmIdentifier {
    AlgorithmIdentifier()
    {
    }

    explicit AlgorithmIdentifier(Vector<int, 9> identifier)
        : identifier(identifier)
    {
    }

    Vector<int, 9> identifier;
    Optional<Vector<int>> ec_parameters;
};

ErrorOr<Vector<int>> parse_ec_parameters(ASN1::Decoder& decoder, Vector<StringView> current_scope = {});

struct BasicConstraints {
    bool is_certificate_authority;
    Crypto::UnsignedBigInteger path_length_constraint;
};

class RelativeDistinguishedName {
public:
    ErrorOr<String> to_string() const;

    ErrorOr<AK::HashSetResult> set(String key, String value)
    {
        return m_members.try_set(move(key), move(value));
    }

    Optional<String const&> get(StringView key) const
    {
        return m_members.get(key);
    }

    Optional<String const&> get(ASN1::AttributeType key) const
    {
        return m_members.get(enum_value(key));
    }

    Optional<String const&> get(ASN1::ObjectClass key) const
    {
        return m_members.get(enum_value(key));
    }

    String common_name() const
    {
        auto entry = get(ASN1::AttributeType::Cn);
        if (entry.has_value()) {
            return entry.value();
        }

        return String();
    }

    String organizational_unit() const
    {
        return get(ASN1::AttributeType::Ou).value_or({});
    }

private:
    HashMap<String, String> m_members;
};

struct Validity {
    UnixDateTime not_before;
    UnixDateTime not_after;
};

class SubjectPublicKey {
public:
    Crypto::PK::RSAPublicKey<Crypto::UnsignedBigInteger> rsa;
    Crypto::PK::ECPublicKey<Crypto::UnsignedBigInteger> ec;

    AlgorithmIdentifier algorithm;
    ByteBuffer raw_key;
};
ErrorOr<SubjectPublicKey> parse_subject_public_key_info(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope = {});

// https://www.rfc-editor.org/rfc/rfc5208#section-5
class PrivateKey {
public:
    Crypto::PK::RSAPrivateKey<Crypto::UnsignedBigInteger> rsa;
    Crypto::PK::ECPrivateKey<Crypto::UnsignedBigInteger> ec;

    AlgorithmIdentifier algorithm;
    ByteBuffer raw_key;

    // FIXME: attributes [0]  IMPLICIT Attributes OPTIONAL
};
ErrorOr<PrivateKey> parse_private_key_info(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope = {});

class Certificate {
public:
    u16 version { 0 };
    AlgorithmIdentifier algorithm;
    SubjectPublicKey public_key;
    ByteBuffer exponent {};
    Crypto::PK::RSAPrivateKey<Crypto::UnsignedBigInteger> private_key {};
    RelativeDistinguishedName issuer, subject;
    Validity validity {};
    Vector<String> SAN;
    Vector<String> IAN;
    u8* ocsp { nullptr };
    Crypto::UnsignedBigInteger serial_number;
    ByteBuffer sign_key {};
    ByteBuffer fingerprint {};
    ByteBuffer der {};
    ByteBuffer data {};
    AlgorithmIdentifier signature_algorithm;
    ByteBuffer signature_value {};
    ByteBuffer original_asn1 {};
    ByteBuffer tbs_asn1 {};
    bool is_allowed_to_sign_certificate { false };
    bool is_certificate_authority { false };
    Optional<size_t> path_length_constraint {};
    bool is_self_issued { false };

    static ErrorOr<Certificate> parse_certificate(ReadonlyBytes, bool client_cert = false);

    bool is_self_signed();
    bool is_valid() const;

private:
    Optional<bool> m_is_self_signed;
};

}
