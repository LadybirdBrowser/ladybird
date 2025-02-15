/*
 * Copyright (c) 2020-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/PK/EC.h>
#include <LibCrypto/PK/RSA.h>

namespace Crypto::Certificate {

struct AlgorithmIdentifier {
    AlgorithmIdentifier()
    {
    }

    explicit AlgorithmIdentifier(Vector<int, 9> const& identifier)
        : identifier(identifier)
    {
    }

    Vector<int, 9> identifier;
    Optional<Vector<int>> ec_parameters {};
};

ErrorOr<Vector<int>> parse_ec_parameters(ASN1::Decoder& decoder, Vector<StringView> current_scope = {});

// https://datatracker.ietf.org/doc/html/rfc5280#section-4.1
class SubjectPublicKey {
public:
    PK::RSAPublicKey<> rsa;
    PK::ECPublicKey<> ec;

    AlgorithmIdentifier algorithm;
    ByteBuffer raw_key;
};
ErrorOr<SubjectPublicKey> parse_subject_public_key_info(ASN1::Decoder& decoder, Vector<StringView> current_scope = {});

// https://www.rfc-editor.org/rfc/rfc5208#section-5
class PrivateKey {
public:
    PK::RSAPrivateKey<> rsa;
    PK::ECPrivateKey<> ec;

    AlgorithmIdentifier algorithm;
    ByteBuffer raw_key;

    // FIXME: attributes [0]  IMPLICIT Attributes OPTIONAL
};
ErrorOr<PrivateKey> parse_private_key_info(ASN1::Decoder& decoder, Vector<StringView> current_scope = {});

}
