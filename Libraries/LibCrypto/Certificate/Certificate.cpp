/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/IPv4Address.h>
#include <LibCore/DateTime.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Certificate/Certificate.h>
#include <LibCrypto/PK/EC.h>

namespace Crypto::Certificate {

ErrorOr<Vector<int>> parse_ec_parameters(ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // ECParameters ::= CHOICE {
    //     namedCurve      OBJECT IDENTIFIER
    // }
    PUSH_SCOPE("ECParameters"sv);
    READ_OBJECT(ObjectIdentifier, Vector<int>, named_curve);
    POP_SCOPE();

    constexpr static Array<Span<int const>, 3> known_curve_identifiers {
        ASN1::secp256r1_oid,
        ASN1::secp384r1_oid,
        ASN1::secp521r1_oid
    };

    bool is_known_curve = false;
    for (auto const& curves : known_curve_identifiers) {
        if (curves == named_curve.span()) {
            is_known_curve = true;
            break;
        }
    }

    if (!is_known_curve) {
        ERROR_WITH_SCOPE(TRY(String::formatted("Unknown named curve {}", named_curve)));
    }

    return named_curve;
}

static ErrorOr<AlgorithmIdentifier> parse_algorithm_identifier(ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // AlgorithmIdentifier{ALGORITHM:SupportedAlgorithms} ::= SEQUENCE {
    //     algorithm ALGORITHM.&id({SupportedAlgorithms}),
    //     parameters ALGORITHM.&Type({SupportedAlgorithms}{@algorithm}) OPTIONAL,
    // ... }
    ENTER_TYPED_SCOPE(Sequence, "AlgorithmIdentifier"sv);
    PUSH_SCOPE("algorithm"sv);
    READ_OBJECT(ObjectIdentifier, Vector<int>, algorithm);
    POP_SCOPE();

    constexpr static Array<Span<int const>, 13> known_algorithm_identifiers {
        ASN1::rsa_encryption_oid,
        ASN1::rsa_md5_encryption_oid,
        ASN1::rsa_sha1_encryption_oid,
        ASN1::rsa_sha256_encryption_oid,
        ASN1::rsa_sha384_encryption_oid,
        ASN1::rsa_sha512_encryption_oid,
        ASN1::ecdsa_with_sha256_encryption_oid,
        ASN1::ecdsa_with_sha384_encryption_oid,
        ASN1::ec_public_key_encryption_oid,
        ASN1::x25519_oid,
        ASN1::ed25519_oid,
        ASN1::x448_oid,
        ASN1::ed448_oid,
    };

    bool is_known_algorithm = false;
    for (auto const& inner : known_algorithm_identifiers) {
        if (inner == algorithm.span()) {
            is_known_algorithm = true;
            break;
        }
    }

    if (!is_known_algorithm) {
        ERROR_WITH_SCOPE(TRY(String::formatted("Unknown algorithm {}", algorithm)));
    }

    // -- When the following OIDs are used in an AlgorithmIdentifier, the
    // -- parameters MUST be present and MUST be NULL.
    //      sha256WithRSAEncryption  OBJECT IDENTIFIER  ::=  { pkcs-1 1 }
    //      sha256WithRSAEncryption  OBJECT IDENTIFIER  ::=  { pkcs-1 4 }
    //      sha256WithRSAEncryption  OBJECT IDENTIFIER  ::=  { pkcs-1 5 }
    //      sha256WithRSAEncryption  OBJECT IDENTIFIER  ::=  { pkcs-1 11 }
    //      sha384WithRSAEncryption  OBJECT IDENTIFIER  ::=  { pkcs-1 12 }
    //      sha512WithRSAEncryption  OBJECT IDENTIFIER  ::=  { pkcs-1 13 }
    //      sha224WithRSAEncryption  OBJECT IDENTIFIER  ::=  { pkcs-1 14 }
    constexpr static Array<Span<int const>, 8> rsa_null_algorithms = {
        ASN1::rsa_encryption_oid,
        ASN1::rsa_md5_encryption_oid,
        ASN1::rsa_sha1_encryption_oid,
        ASN1::rsa_sha256_encryption_oid,
        ASN1::rsa_sha384_encryption_oid,
        ASN1::rsa_sha512_encryption_oid,
        ASN1::rsa_sha224_encryption_oid,
    };

    bool is_rsa_null_algorithm = false;
    for (auto const& inner : rsa_null_algorithms) {
        if (inner == algorithm.span()) {
            is_rsa_null_algorithm = true;
            break;
        }
    }

    if (is_rsa_null_algorithm) {
        PUSH_SCOPE("RSA null parameter"sv);
        READ_OBJECT(Null, void*, forced_null);
        (void)forced_null;
        POP_SCOPE();

        EXIT_SCOPE();
        return AlgorithmIdentifier(algorithm);
    }

    // https://www.ietf.org/rfc/rfc5758.txt
    // When the ecdsa-with-SHA224, ecdsa-with-SHA256, ecdsa-with-SHA384, or
    // ecdsa-with-SHA512 algorithm identifier appears in the algorithm field
    // as an AlgorithmIdentifier, the encoding MUST omit the parameters
    // field.

    // https://datatracker.ietf.org/doc/html/rfc8410#section-9
    // For all of the OIDs, the parameters MUST be absent.
    constexpr static Array<Span<int const>, 8> no_parameter_algorithms = {
        ASN1::ecdsa_with_sha224_encryption_oid,
        ASN1::ecdsa_with_sha256_encryption_oid,
        ASN1::ecdsa_with_sha384_encryption_oid,
        ASN1::ecdsa_with_sha512_encryption_oid,
        ASN1::x25519_oid,
        ASN1::x448_oid,
        ASN1::ed25519_oid,
        ASN1::ed448_oid
    };

    bool is_no_parameter_algorithm = false;
    for (auto const& inner : no_parameter_algorithms) {
        if (inner == algorithm.span()) {
            is_no_parameter_algorithm = true;
        }
    }

    if (is_no_parameter_algorithm) {
        EXIT_SCOPE();

        return AlgorithmIdentifier(algorithm);
    }

    if (algorithm.span() == ASN1::ec_public_key_encryption_oid.span()) {
        // The parameters associated with id-ecPublicKey SHOULD be absent or ECParameters,
        // and NULL is allowed to support legacy implementations.
        if (decoder.eof()) {
            EXIT_SCOPE();

            return AlgorithmIdentifier(algorithm);
        }

        auto tag = TRY(decoder.peek());
        if (tag.kind == Crypto::ASN1::Kind::Null) {
            PUSH_SCOPE("ecPublicKey null parameter"sv);
            READ_OBJECT(Null, void*, forced_null);
            (void)forced_null;
            POP_SCOPE();

            EXIT_SCOPE();
            return AlgorithmIdentifier(algorithm);
        }

        auto algorithm_identifier = AlgorithmIdentifier(algorithm);
        algorithm_identifier.ec_parameters = TRY(parse_ec_parameters(decoder, current_scope));

        EXIT_SCOPE();
        return algorithm_identifier;
    }

    ERROR_WITH_SCOPE(TRY(String::formatted("Unhandled parameters for algorithm {}", algorithm)));
}

// https://datatracker.ietf.org/doc/html/rfc5280#section-4.1
ErrorOr<SubjectPublicKey> parse_subject_public_key_info(ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // SubjectPublicKeyInfo ::= Sequence {
    //     algorithm           AlgorithmIdentifier,
    //     subject_public_key  BitString
    // }

    SubjectPublicKey public_key;
    ENTER_TYPED_SCOPE(Sequence, "SubjectPublicKeyInfo"sv);

    public_key.algorithm = TRY(parse_algorithm_identifier(decoder, current_scope));

    PUSH_SCOPE("subjectPublicKey"sv);
    READ_OBJECT(BitString, Crypto::ASN1::BitStringView, value);
    POP_SCOPE();

    public_key.raw_key = TRY(ByteBuffer::copy(TRY(value.raw_bytes())));

    if (public_key.algorithm.identifier.span() == ASN1::rsa_encryption_oid.span()) {
        auto maybe_key = Crypto::PK::RSA::parse_rsa_key(public_key.raw_key, false, current_scope);
        if (maybe_key.is_error()) {
            ERROR_WITH_SCOPE(maybe_key.release_error());
        }

        public_key.rsa = move(maybe_key.release_value().public_key);

        EXIT_SCOPE();
        return public_key;
    }
    if (public_key.algorithm.identifier.span() == ASN1::ec_public_key_encryption_oid.span()) {
        auto maybe_key = Crypto::PK::EC::parse_ec_key(public_key.raw_key, false, current_scope);
        if (maybe_key.is_error()) {
            ERROR_WITH_SCOPE(maybe_key.release_error());
        }

        public_key.ec = move(maybe_key.release_value().public_key);

        EXIT_SCOPE();
        return public_key;
    }

    // https://datatracker.ietf.org/doc/html/rfc8410#section-9
    // For all of the OIDs, the parameters MUST be absent.
    constexpr static Array<Span<int const>, 5> no_parameter_algorithms = {
        ASN1::ec_public_key_encryption_oid,
        ASN1::x25519_oid,
        ASN1::x448_oid,
        ASN1::ed25519_oid,
        ASN1::ed448_oid
    };

    for (auto const& inner : no_parameter_algorithms) {
        if (public_key.algorithm.identifier.span() == inner) {
            // Note: Raw key is already stored, so we can just exit out at this point.
            EXIT_SCOPE();
            return public_key;
        }
    }

    String algo_oid = TRY(String::join("."sv, public_key.algorithm.identifier));
    ERROR_WITH_SCOPE(TRY(String::formatted("Unhandled algorithm {}", algo_oid)));
}

// https://www.rfc-editor.org/rfc/rfc5208#section-5
ErrorOr<PrivateKey> parse_private_key_info(ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // PrivateKeyInfo ::= SEQUENCE {
    //     version                   Version,
    //     privateKeyAlgorithm       PrivateKeyAlgorithmIdentifier,
    //     privateKey                PrivateKey,
    //     attributes           [0]  IMPLICIT Attributes OPTIONAL
    //  }

    PrivateKey private_key;
    ENTER_TYPED_SCOPE(Sequence, "PrivateKeyInfo"sv);

    READ_OBJECT(Integer, Crypto::UnsignedBigInteger, version);
    if (version != 0) {
        ERROR_WITH_SCOPE(TRY(String::formatted("Invalid version value at {}", current_scope)));
    }
    private_key.algorithm = TRY(parse_algorithm_identifier(decoder, current_scope));

    PUSH_SCOPE("privateKey"sv);
    READ_OBJECT(OctetString, StringView, value);
    POP_SCOPE();

    private_key.raw_key = TRY(ByteBuffer::copy(value.bytes()));

    if (private_key.algorithm.identifier.span() == ASN1::rsa_encryption_oid.span()) {
        auto maybe_key = Crypto::PK::RSA::parse_rsa_key(value.bytes(), true, current_scope);
        if (maybe_key.is_error()) {
            ERROR_WITH_SCOPE(maybe_key.release_error());
        }

        private_key.rsa = move(maybe_key.release_value().private_key);

        EXIT_SCOPE();
        return private_key;
    }
    if (private_key.algorithm.identifier.span() == ASN1::ec_public_key_encryption_oid.span()) {
        auto maybe_key = Crypto::PK::EC::parse_ec_key(value.bytes(), true, current_scope);
        if (maybe_key.is_error()) {
            ERROR_WITH_SCOPE(maybe_key.release_error());
        }

        private_key.ec = move(maybe_key.release_value().private_key);

        EXIT_SCOPE();
        return private_key;
    }

    // https://datatracker.ietf.org/doc/html/rfc8410#section-9
    // For all of the OIDs, the parameters MUST be absent.
    constexpr static Array<Span<int const>, 5> no_parameter_algorithms = {
        ASN1::ec_public_key_encryption_oid,
        ASN1::x25519_oid,
        ASN1::x448_oid,
        ASN1::ed25519_oid,
        ASN1::ed448_oid
    };

    for (auto const& inner : no_parameter_algorithms) {
        if (private_key.algorithm.identifier.span() == inner) {
            // Note: Raw key is already stored, so we can just exit out at this point.
            EXIT_SCOPE();
            return private_key;
        }
    }

    String algo_oid = TRY(String::join("."sv, private_key.algorithm.identifier));
    ERROR_WITH_SCOPE(TRY(String::formatted("Unhandled algorithm {}", algo_oid)));
}

}
