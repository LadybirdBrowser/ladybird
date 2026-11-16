/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/PK/EC.h>
#include <LibCrypto/PK/MLDSA.h>
#include <LibCrypto/PK/MLKEM.h>
#include <LibCrypto/PK/RSA.h>

// NB: Each parser reads one DER structure from the start of its input, and gives the number of bytes it took in
//     `consumed`. A structure that is not in DER, or whose key OpenSSL cannot decode, is an error.
namespace Crypto::PK {

struct AlgorithmIdentifier {
    ASN1::ObjectIdentifier identifier;
    // The parameters, when they are an object identifier, such as the named curve of an EC key.
    Optional<ASN1::ObjectIdentifier> ec_parameters;
};

// https://www.rfc-editor.org/rfc/rfc5280#section-4.1
struct SubjectPublicKeyInfo {
    AlgorithmIdentifier algorithm;
    ByteBuffer raw_key;
    RSAPublicKey rsa;
    ECPublicKey ec;
};

// https://www.rfc-editor.org/rfc/rfc5208#section-5
struct PrivateKeyInfo {
    AlgorithmIdentifier algorithm;
    ByteBuffer raw_key;
    RSAPrivateKey rsa;
    ECPrivateKey ec;
    MLDSAPrivateKey mldsa;
    MLKEMPrivateKey mlkem;
};

ErrorOr<SubjectPublicKeyInfo> parse_subject_public_key_info(ReadonlyBytes, size_t& consumed);
ErrorOr<PrivateKeyInfo> parse_private_key_info(ReadonlyBytes, size_t& consumed);
ErrorOr<ReadonlyBytes> parse_octet_string(ReadonlyBytes, size_t& consumed);

}
