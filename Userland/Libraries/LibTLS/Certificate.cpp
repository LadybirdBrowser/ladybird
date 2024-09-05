/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Certificate.h"
#include <AK/Debug.h>
#include <AK/IPv4Address.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/ASN1/PEM.h>

namespace {
static String s_error_string;
}

namespace TLS {

#define ERROR_WITH_SCOPE(error)                                                  \
    do {                                                                         \
        s_error_string = TRY(String::formatted("{}: {}", current_scope, error)); \
        return Error::from_string_view(s_error_string.bytes_as_string_view());   \
    } while (0)

#define ENTER_TYPED_SCOPE(tag_kind_name, scope)                                                                                                               \
    do {                                                                                                                                                      \
        if (auto tag = decoder.peek(); tag.is_error() || tag.value().kind != Crypto::ASN1::Kind::tag_kind_name) {                                             \
            if (tag.is_error())                                                                                                                               \
                ERROR_WITH_SCOPE(TRY(String::formatted(scope " data was invalid: {}", tag.error())));                                                         \
            else                                                                                                                                              \
                ERROR_WITH_SCOPE(TRY(String::formatted(scope " data was not of kind " #tag_kind_name " was {}", Crypto::ASN1::kind_name(tag.value().kind)))); \
        }                                                                                                                                                     \
        ENTER_SCOPE(scope);                                                                                                                                   \
    } while (0)

#define ENTER_SCOPE(scope)                                                                \
    do {                                                                                  \
        if (auto result = decoder.enter(); result.is_error()) {                           \
            ERROR_WITH_SCOPE(TRY(String::formatted("Failed to enter scope: {}", scope))); \
        }                                                                                 \
        PUSH_SCOPE(scope)                                                                 \
    } while (0)

#define PUSH_SCOPE(scope) current_scope.append(#scope##sv);

#define EXIT_SCOPE()                                                                             \
    do {                                                                                         \
        if (auto error = decoder.leave(); error.is_error()) {                                    \
            ERROR_WITH_SCOPE(TRY(String::formatted("Failed to exit scope: {}", error.error()))); \
        }                                                                                        \
        POP_SCOPE();                                                                             \
    } while (0)

#define POP_SCOPE() current_scope.remove(current_scope.size() - 1);

#define READ_OBJECT(kind_name, type_name, value_name)                                                                    \
    auto value_name##_result = decoder.read<type_name>(Crypto::ASN1::Class::Universal, Crypto::ASN1::Kind::kind_name);   \
    if (value_name##_result.is_error()) {                                                                                \
        ERROR_WITH_SCOPE(TRY(String::formatted("Read of kind " #kind_name " failed: {}", value_name##_result.error()))); \
    }                                                                                                                    \
    auto value_name = value_name##_result.release_value();

#define REWRITE_TAG(kind_name)                                                                                              \
    auto value_name##_result = decoder.rewrite_tag(Crypto::ASN1::Kind::kind_name);                                          \
    if (value_name##_result.is_error()) {                                                                                   \
        ERROR_WITH_SCOPE(TRY(String::formatted("Rewrite of kind " #kind_name " failed: {}", value_name##_result.error()))); \
    }

#define DROP_OBJECT()                                                                   \
    do {                                                                                \
        if (auto error = decoder.drop(); error.is_error()) {                            \
            ERROR_WITH_SCOPE(TRY(String::formatted("Drop failed: {}", error.error()))); \
        }                                                                               \
    } while (0)

static ErrorOr<SupportedGroup> oid_to_curve(Vector<int> curve)
{
    if (curve == curve_ansip384r1)
        return SupportedGroup::SECP384R1;
    else if (curve == curve_prime256)
        return SupportedGroup::SECP256R1;

    return Error::from_string_literal("Unknown curve oid");
}

static ErrorOr<Crypto::UnsignedBigInteger> parse_certificate_version(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // Version ::= INTEGER {v1(0), v2(1), v3(2)}
    if (auto tag = decoder.peek(); !tag.is_error() && tag.value().type == Crypto::ASN1::Type::Constructed) {
        ENTER_SCOPE("Version"sv);
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, version);
        if (version > 3) {
            ERROR_WITH_SCOPE(TRY(String::formatted("Invalid version value at {}", current_scope)));
        }
        EXIT_SCOPE();
        return version;
    } else {
        return Crypto::UnsignedBigInteger { 0 };
    }
}

static ErrorOr<Crypto::UnsignedBigInteger> parse_serial_number(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // CertificateSerialNumber ::= INTEGER
    PUSH_SCOPE("CertificateSerialNumber"sv);
    READ_OBJECT(Integer, Crypto::UnsignedBigInteger, serial);
    POP_SCOPE();
    return serial;
}

static ErrorOr<SupportedGroup> parse_ec_parameters(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // ECParameters ::= CHOICE {
    //     namedCurve      OBJECT IDENTIFIER
    // }
    PUSH_SCOPE("ECParameters"sv);
    READ_OBJECT(ObjectIdentifier, Vector<int>, named_curve);
    // Note: namedCurve sometimes has 5 nodes, but we need 7 for the comparison below to work.
    while (named_curve.size() < 7) {
        named_curve.append(0);
    }
    POP_SCOPE();

    bool is_known_curve = false;
    for (auto const& curves : known_curve_identifiers) {
        if (curves.span() == named_curve.span()) {
            is_known_curve = true;
            break;
        }
    }

    if (!is_known_curve) {
        ERROR_WITH_SCOPE(TRY(String::formatted("Unknown named curve {}", named_curve)));
    }

    return oid_to_curve(named_curve);
}

static ErrorOr<AlgorithmIdentifier> parse_algorithm_identifier(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // AlgorithmIdentifier{ALGORITHM:SupportedAlgorithms} ::= SEQUENCE {
    //     algorithm ALGORITHM.&id({SupportedAlgorithms}),
    //     parameters ALGORITHM.&Type({SupportedAlgorithms}{@algorithm}) OPTIONAL,
    // ... }
    ENTER_TYPED_SCOPE(Sequence, "AlgorithmIdentifier"sv);
    PUSH_SCOPE("algorithm"sv);
    READ_OBJECT(ObjectIdentifier, Vector<int>, algorithm);
    // Note: ecPublicKey only has 6 nodes, but we need 7 for the comparison below to work.
    while (algorithm.size() < 7) {
        algorithm.append(0);
    }
    POP_SCOPE();

    bool is_known_algorithm = false;
    for (auto const& inner : known_algorithm_identifiers) {
        if (inner.span() == algorithm.span()) {
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
    Array<Array<int, 7>, 8> rsa_null_algorithms = {
        rsa_encryption_oid,
        rsa_md5_encryption_oid,
        rsa_sha1_encryption_oid,
        rsa_sha256_encryption_oid,
        rsa_sha384_encryption_oid,
        rsa_sha512_encryption_oid,
        rsa_sha224_encryption_oid,
    };

    bool is_rsa_null_algorithm = false;
    for (auto const& inner : rsa_null_algorithms) {
        if (inner.span() == algorithm.span()) {
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

    // When the ecdsa-with-SHA224, ecdsa-with-SHA256, ecdsa-with-SHA384, or
    // ecdsa-with-SHA512 algorithm identifier appears in the algorithm field
    // as an AlgorithmIdentifier, the encoding MUST omit the parameters
    // field.
    Array<Array<int, 7>, 8> no_parameter_algorithms = {
        ecdsa_with_sha224_encryption_oid,
        ecdsa_with_sha256_encryption_oid,
        ecdsa_with_sha384_encryption_oid,
        ecdsa_with_sha512_encryption_oid,
    };

    bool is_no_parameter_algorithm = false;
    for (auto const& inner : no_parameter_algorithms) {
        if (inner.span() == algorithm.span()) {
            is_no_parameter_algorithm = true;
        }
    }

    if (is_no_parameter_algorithm) {
        EXIT_SCOPE();

        return AlgorithmIdentifier(algorithm);
    }

    if (algorithm.span() == ec_public_key_encryption_oid.span()) {
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

static ErrorOr<RelativeDistinguishedName> parse_name(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    RelativeDistinguishedName rdn {};
    // Name ::= Choice {
    //     rdn_sequence RDNSequence
    // } // NOTE: since this is the only alternative, there's no index
    // RDNSequence ::= Sequence OF RelativeDistinguishedName
    ENTER_TYPED_SCOPE(Sequence, "Name"sv);
    while (!decoder.eof()) {
        // RelativeDistinguishedName ::= Set OF AttributeTypeAndValue
        ENTER_TYPED_SCOPE(Set, "RDNSequence"sv);
        while (!decoder.eof()) {
            // AttributeTypeAndValue ::= Sequence {
            //     type   AttributeType,
            //     value  AttributeValue
            // }
            ENTER_TYPED_SCOPE(Sequence, "AttributeTypeAndValue"sv);
            // AttributeType ::= ObjectIdentifier
            PUSH_SCOPE("AttributeType"sv)
            READ_OBJECT(ObjectIdentifier, Vector<int>, attribute_type_oid);
            POP_SCOPE();

            // AttributeValue ::= Any
            PUSH_SCOPE("AttributeValue"sv)
            READ_OBJECT(PrintableString, StringView, attribute_value);
            POP_SCOPE();

            auto attribute_type_string = TRY(String::join("."sv, attribute_type_oid));
            auto attribute_value_string = TRY(String::from_utf8(attribute_value));
            TRY(rdn.set(move(attribute_type_string), move(attribute_value_string)));

            EXIT_SCOPE();
        }
        EXIT_SCOPE();
    }
    EXIT_SCOPE();

    return rdn;
}

static ErrorOr<UnixDateTime> parse_time(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // Time ::= Choice {
    //     utc_time     UTCTime,
    //     general_time GeneralizedTime
    // }
    auto tag = TRY(decoder.peek());
    if (tag.kind == Crypto::ASN1::Kind::UTCTime) {
        PUSH_SCOPE("UTCTime"sv);

        READ_OBJECT(UTCTime, StringView, utc_time);
        auto parse_result = Crypto::ASN1::parse_utc_time(utc_time);
        if (!parse_result.has_value()) {
            ERROR_WITH_SCOPE(TRY(String::formatted("Failed to parse UTCTime {}", utc_time)));
        }

        POP_SCOPE();
        return parse_result.release_value();
    }

    if (tag.kind == Crypto::ASN1::Kind::GeneralizedTime) {
        PUSH_SCOPE("GeneralizedTime"sv);

        READ_OBJECT(UTCTime, StringView, generalized_time);
        auto parse_result = Crypto::ASN1::parse_generalized_time(generalized_time);
        if (!parse_result.has_value()) {
            ERROR_WITH_SCOPE(TRY(String::formatted("Failed to parse GeneralizedTime {}", generalized_time)));
        }

        POP_SCOPE();
        return parse_result.release_value();
    }

    ERROR_WITH_SCOPE(TRY(String::formatted("Unrecognised Time format {}", kind_name(tag.kind))));
}

static ErrorOr<Validity> parse_validity(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    Validity validity {};

    // Validity ::= SEQUENCE {
    //     notBefore      Time,
    //     notAfter       Time  }
    ENTER_TYPED_SCOPE(Sequence, "Validity"sv);

    validity.not_before = TRY(parse_time(decoder, current_scope));
    validity.not_after = TRY(parse_time(decoder, current_scope));

    EXIT_SCOPE();

    return validity;
}

ErrorOr<SubjectPublicKey> parse_subject_public_key_info(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
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

    if (public_key.algorithm.identifier.span() == rsa_encryption_oid.span()) {
        auto key = Crypto::PK::RSA::parse_rsa_key(TRY(value.raw_bytes()));
        if (!key.public_key.length()) {
            return Error::from_string_literal("Invalid RSA key");
        }

        public_key.rsa = move(key.public_key);

        EXIT_SCOPE();
        return public_key;
    }

    if (public_key.algorithm.identifier.span() == ec_public_key_encryption_oid.span()) {
        // Note: Raw key is already stored, so we can just exit out at this point.
        EXIT_SCOPE();
        return public_key;
    }

    String algo_oid = TRY(String::join("."sv, public_key.algorithm.identifier));
    ERROR_WITH_SCOPE(TRY(String::formatted("Unhandled algorithm {}", algo_oid)));
}

// https://www.rfc-editor.org/rfc/rfc5208#section-5
ErrorOr<PrivateKey> parse_private_key_info(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
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

    if (private_key.algorithm.identifier.span() == rsa_encryption_oid.span()) {
        auto key = Crypto::PK::RSA::parse_rsa_key(value.bytes());
        if (key.private_key.length() == 0) {
            ERROR_WITH_SCOPE(TRY(String::formatted("Invalid RSA key at {}", current_scope)));
        }

        private_key.rsa = move(key.private_key);

        EXIT_SCOPE();
        return private_key;
    }

    if (private_key.algorithm.identifier.span() == ec_public_key_encryption_oid.span()) {
        // Note: Raw key is already stored, so we can just exit out at this point.
        EXIT_SCOPE();
        return private_key;
    }

    String algo_oid = TRY(String::join("."sv, private_key.algorithm.identifier));
    ERROR_WITH_SCOPE(TRY(String::formatted("Unhandled algorithm {}", algo_oid)));
}

static ErrorOr<Crypto::ASN1::BitStringView> parse_unique_identifier(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // UniqueIdentifier  ::=  BIT STRING
    PUSH_SCOPE("UniqueIdentifier"sv);
    READ_OBJECT(BitString, Crypto::ASN1::BitStringView, value);
    POP_SCOPE();

    return value;
}

static ErrorOr<String> parse_general_name(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // GeneralName ::= CHOICE {
    //     otherName                    [0] INSTANCE OF OTHER-NAME,
    //     rfc822Name                   [1] IA5String,
    //     dNSName                      [2] IA5String,
    //     x400Address                  [3] ORAddress,
    //     directoryName                [4] Name,
    //     ediPartyName                 [5] EDIPartyName,
    //     uniformResourceIdentifier    [6] IA5String,
    //     iPAddress                    [7] OCTET STRING,
    //     registeredID                 [8] OBJECT IDENTIFIER,
    // }
    auto tag = TRY(decoder.peek());
    auto tag_value = static_cast<u8>(tag.kind);
    switch (tag_value) {
    case 0:
        // Note: We don't know how to use this.
        PUSH_SCOPE("otherName"sv)
        DROP_OBJECT();
        POP_SCOPE();
        break;
    case 1: {
        PUSH_SCOPE("rfc822Name"sv)
        READ_OBJECT(IA5String, StringView, name);
        POP_SCOPE();
        return String::from_utf8(name);
    }
    case 2: {
        PUSH_SCOPE("dNSName"sv)
        READ_OBJECT(IA5String, StringView, name);
        POP_SCOPE();
        return String::from_utf8(name);
    }
    case 3:
        // Note: We don't know how to use this.
        PUSH_SCOPE("x400Address"sv)
        DROP_OBJECT();
        POP_SCOPE();
        break;
    case 4: {
        PUSH_SCOPE("directoryName"sv);
        READ_OBJECT(OctetString, StringView, directory_name);
        Crypto::ASN1::Decoder decoder { directory_name.bytes() };
        auto names = TRY(parse_name(decoder, current_scope));
        POP_SCOPE();
        return names.to_string();
    }
    case 5:
        // Note: We don't know how to use this.
        PUSH_SCOPE("ediPartyName");
        DROP_OBJECT();
        POP_SCOPE();
        break;
    case 6: {
        PUSH_SCOPE("uniformResourceIdentifier"sv);
        READ_OBJECT(IA5String, StringView, name);
        POP_SCOPE();
        return String::from_utf8(name);
    }
    case 7: {
        PUSH_SCOPE("iPAddress"sv);
        READ_OBJECT(OctetString, StringView, ip_addr_sv);
        IPv4Address ip_addr { ip_addr_sv.bytes().data() };
        POP_SCOPE();
        return ip_addr.to_string();
    }
    case 8: {
        PUSH_SCOPE("registeredID"sv);
        READ_OBJECT(ObjectIdentifier, Vector<int>, identifier);
        POP_SCOPE();
        return String::join("."sv, identifier);
    }
    default:
        ERROR_WITH_SCOPE("Unknown tag in GeneralNames choice"sv);
    }

    ERROR_WITH_SCOPE("Unknown tag in GeneralNames choice"sv);
}

static ErrorOr<Vector<String>> parse_general_names(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // GeneralNames ::= Sequence OF GeneralName
    ENTER_TYPED_SCOPE(Sequence, "GeneralNames");

    Vector<String> names;
    while (!decoder.eof()) {
        names.append(TRY(parse_general_name(decoder, current_scope)));
    }

    EXIT_SCOPE();

    return names;
}

static ErrorOr<Vector<String>> parse_subject_alternative_names(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // SubjectAlternativeName ::= GeneralNames
    PUSH_SCOPE("SubjectAlternativeName"sv);
    auto values = TRY(parse_general_names(decoder, current_scope));
    POP_SCOPE();

    return values;
}

static ErrorOr<Vector<String>> parse_issuer_alternative_names(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // issuerAltName ::= GeneralNames
    PUSH_SCOPE("issuerAltName"sv);
    auto values = TRY(parse_general_names(decoder, current_scope));
    POP_SCOPE();

    return values;
}

static ErrorOr<Crypto::ASN1::BitStringView> parse_key_usage(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // KeyUsage ::= BIT STRING {
    //     digitalSignature        (0),
    //     contentCommitment       (1),
    //     keyEncipherment         (2),
    //     dataEncipherment        (3),
    //     keyAgreement            (4),
    //     keyCertSign             (5),
    //     cRLSign                 (6),
    //     encipherOnly            (7),
    //     decipherOnly            (8)
    // }

    PUSH_SCOPE("KeyUsage"sv);
    READ_OBJECT(BitString, Crypto::ASN1::BitStringView, usage);
    POP_SCOPE();

    return usage;
}

static ErrorOr<BasicConstraints> parse_basic_constraints(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // BasicConstraints ::= SEQUENCE {
    //     cA                      BOOLEAN DEFAULT FALSE,
    //     pathLenConstraint       INTEGER (0..MAX) OPTIONAL
    // }

    BasicConstraints constraints {};

    ENTER_TYPED_SCOPE(Sequence, "BasicConstraints"sv);

    if (decoder.eof()) {
        EXIT_SCOPE();
        return constraints;
    }

    auto ca_tag = TRY(decoder.peek());
    if (ca_tag.kind == Crypto::ASN1::Kind::Boolean) {
        PUSH_SCOPE("cA"sv);
        READ_OBJECT(Boolean, bool, is_certificate_authority);
        constraints.is_certificate_authority = is_certificate_authority;
        POP_SCOPE();
    }

    if (decoder.eof()) {
        EXIT_SCOPE();
        return constraints;
    }

    auto path_length_tag = TRY(decoder.peek());
    if (path_length_tag.kind == Crypto::ASN1::Kind::Integer) {
        PUSH_SCOPE("pathLenConstraint"sv);
        READ_OBJECT(Integer, Crypto::UnsignedBigInteger, path_length_constraint);
        constraints.path_length_constraint = path_length_constraint;
        POP_SCOPE();
    }

    EXIT_SCOPE();
    return constraints;
}

static ErrorOr<void> parse_extension(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope, Certificate& certificate)
{
    // Extension ::= Sequence {
    //     extension_id     ObjectIdentifier,
    //     critical         Boolean DEFAULT false,
    //     extension_value  OctetString (DER-encoded)
    // }
    ENTER_TYPED_SCOPE(Sequence, "Extension"sv);

    PUSH_SCOPE("extension_id"sv);
    READ_OBJECT(ObjectIdentifier, Vector<int>, extension_id);
    POP_SCOPE();

    bool is_critical = false;
    auto peek = TRY(decoder.peek());
    if (peek.kind == Crypto::ASN1::Kind::Boolean) {
        PUSH_SCOPE("critical"sv);
        READ_OBJECT(Boolean, bool, extension_critical);
        is_critical = extension_critical;
        POP_SCOPE();
    }

    PUSH_SCOPE("extension_value"sv);
    READ_OBJECT(OctetString, StringView, extension_value);
    POP_SCOPE();

    bool is_known_extension = false;

    Crypto::ASN1::Decoder extension_decoder { extension_value.bytes() };
    Vector<StringView, 8> extension_scope {};
    if (extension_id == subject_alternative_name_oid) {
        is_known_extension = true;
        auto alternate_names = TRY(parse_subject_alternative_names(extension_decoder, extension_scope));
        certificate.SAN = alternate_names;
    }

    if (extension_id == key_usage_oid) {
        is_known_extension = true;
        auto usage = TRY(parse_key_usage(extension_decoder, extension_scope));
        certificate.is_allowed_to_sign_certificate = usage.get(5);
    }

    if (extension_id == basic_constraints_oid) {
        is_known_extension = true;
        auto constraints = TRY(parse_basic_constraints(extension_decoder, extension_scope));
        certificate.is_certificate_authority = constraints.is_certificate_authority;
        certificate.path_length_constraint = constraints.path_length_constraint.to_u64();
    }

    if (extension_id == issuer_alternative_name_oid) {
        is_known_extension = true;
        auto alternate_names = TRY(parse_issuer_alternative_names(extension_decoder, extension_scope));
        certificate.IAN = alternate_names;
    }

    EXIT_SCOPE();

    if (is_critical && !is_known_extension) {
        ERROR_WITH_SCOPE(TRY(String::formatted("Extension {} is critical, but we do not support it", extension_id)));
    }

    if (!is_known_extension) {
        dbgln_if(TLS_DEBUG, TRY(String::formatted("{}: Unhandled extension: {}", current_scope, extension_id)));
    }

    return {};
}

static ErrorOr<void> parse_extensions(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope, Certificate& certificate)
{
    // Extensions ::= Sequence OF Extension
    ENTER_TYPED_SCOPE(Sequence, "Extensions"sv);

    while (!decoder.eof()) {
        TRY(parse_extension(decoder, current_scope, certificate));
    }

    EXIT_SCOPE();

    return {};
}

static ErrorOr<Certificate> parse_tbs_certificate(Crypto::ASN1::Decoder& decoder, Vector<StringView> current_scope)
{
    // TBSCertificate ::= SEQUENCE {
    //     version [0] Version DEFAULT v1,
    //     serialNumber CertificateSerialNumber,
    //     signature AlgorithmIdentifier{{SupportedAlgorithms}},
    //     issuer Name,
    //     validity Validity,
    //     subject Name,
    //     subjectPublicKeyInfo SubjectPublicKeyInfo,
    //     issuerUniqueIdentifier [1] IMPLICIT UniqueIdentifier OPTIONAL,
    //     ...,
    //     [[2: -- if present, version shall be v2 or v3
    //     subjectUniqueIdentifier [2] IMPLICIT UniqueIdentifier OPTIONAL]],
    //     [[3: -- if present, version shall be v2 or v3
    //     extensions [3] Extensions OPTIONAL]]
    //     -- If present, version shall be v3]]
    // }

    // Note: Parse out the ASN.1 of this object, since its used for TLS verification.
    // To do this, we get the bytes of our parent, the size of ourself, and slice the parent buffer.
    auto pre_cert_buffer = TRY(decoder.peek_entry_bytes());

    // FIXME: Dont assume this value.
    // Note: we assume this to be 4. 1 for the tag, and 3 for the length.
    auto entry_length_byte_count = 4;

    ENTER_TYPED_SCOPE(Sequence, "TBSCertificate"sv);

    auto post_cert_buffer = TRY(decoder.peek_entry_bytes());
    if (pre_cert_buffer.size() < post_cert_buffer.size() + entry_length_byte_count) {
        ERROR_WITH_SCOPE("Unexpected end of file");
    }

    Certificate certificate;
    certificate.version = TRY(parse_certificate_version(decoder, current_scope)).to_u64();
    certificate.serial_number = TRY(parse_serial_number(decoder, current_scope));
    certificate.algorithm = TRY(parse_algorithm_identifier(decoder, current_scope));
    certificate.issuer = TRY(parse_name(decoder, current_scope));
    certificate.validity = TRY(parse_validity(decoder, current_scope));
    certificate.subject = TRY(parse_name(decoder, current_scope));
    certificate.public_key = TRY(parse_subject_public_key_info(decoder, current_scope));
    certificate.tbs_asn1 = TRY(ByteBuffer::copy(pre_cert_buffer.slice(0, post_cert_buffer.size() + entry_length_byte_count)));

    if (!decoder.eof()) {
        auto tag = TRY(decoder.peek());
        if (static_cast<u8>(tag.kind) == 1) {
            REWRITE_TAG(BitString)
            TRY(parse_unique_identifier(decoder, current_scope));
        }
    }

    if (!decoder.eof()) {
        auto tag = TRY(decoder.peek());
        if (static_cast<u8>(tag.kind) == 2) {
            REWRITE_TAG(BitString)
            TRY(parse_unique_identifier(decoder, current_scope));
        }
    }

    if (!decoder.eof()) {
        auto tag = TRY(decoder.peek());
        if (static_cast<u8>(tag.kind) == 3) {
            REWRITE_TAG(Sequence)
            ENTER_TYPED_SCOPE(Sequence, "extensions"sv);

            TRY(parse_extensions(decoder, current_scope, certificate));

            EXIT_SCOPE();
        }
    }

    if (!decoder.eof()) {
        ERROR_WITH_SCOPE("Reached end of TBS parse with more data left"sv);
    }

    certificate.is_self_issued = TRY(certificate.issuer.to_string()) == TRY(certificate.subject.to_string());

    EXIT_SCOPE();

    return certificate;
}

ErrorOr<Certificate> Certificate::parse_certificate(ReadonlyBytes buffer, bool)
{
    Crypto::ASN1::Decoder decoder { buffer };
    Vector<StringView, 8> current_scope {};

    // Certificate ::= SIGNED{TBSCertificate}

    // SIGNED{ToBeSigned} ::= SEQUENCE {
    //     toBeSigned ToBeSigned,
    //     COMPONENTS OF SIGNATURE{ToBeSigned},
    // }

    // SIGNATURE{ToBeSigned} ::= SEQUENCE {
    //      algorithmIdentifier AlgorithmIdentifier{{SupportedAlgorithms}},
    //      encrypted ENCRYPTED-HASH{ToBeSigned},
    // }

    // ENCRYPTED-HASH{ToBeSigned} ::= BIT STRING (CONSTRAINED BY {
    // -- shall be the result of applying a hashing procedure to the DER-encoded (see 6.2)
    // -- octets of a value of -- ToBeSigned -- and then applying an encipherment procedure
    // -- to those octets -- } )

    ENTER_TYPED_SCOPE(Sequence, "Certificate"sv);

    Certificate certificate = TRY(parse_tbs_certificate(decoder, current_scope));
    certificate.original_asn1 = TRY(ByteBuffer::copy(buffer));

    certificate.signature_algorithm = TRY(parse_algorithm_identifier(decoder, current_scope));

    PUSH_SCOPE("signature"sv);
    READ_OBJECT(BitString, Crypto::ASN1::BitStringView, signature);
    certificate.signature_value = TRY(ByteBuffer::copy(TRY(signature.raw_bytes())));
    POP_SCOPE();

    if (!decoder.eof()) {
        ERROR_WITH_SCOPE("Reached end of Certificate parse with more data left"sv);
    }

    EXIT_SCOPE();

    return certificate;
}

#undef PUSH_SCOPE
#undef ENTER_SCOPE
#undef ENTER_TYPED_SCOPE
#undef POP_SCOPE
#undef EXIT_SCOPE
#undef READ_OBJECT
#undef DROP_OBJECT
#undef REWRITE_TAG

ErrorOr<String> RelativeDistinguishedName::to_string() const
{
#define ADD_IF_RECOGNIZED(identifier, shorthand_code)         \
    if (member_identifier == identifier) {                    \
        cert_name.appendff("\\{}={}", shorthand_code, value); \
        continue;                                             \
    }

    StringBuilder cert_name;

    for (auto const& [member_identifier, value] : m_members) {
        ADD_IF_RECOGNIZED(enum_value(AttributeType::SerialNumber), "SERIALNUMBER");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::Email), "MAIL");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::Title), "T");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::PostalCode), "PC");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::DnQualifier), "DNQ");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::GivenName), "GIVENNAME");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::Surname), "SN");

        ADD_IF_RECOGNIZED(enum_value(AttributeType::Cn), "CN");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::L), "L");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::St), "ST");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::O), "O");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::Ou), "OU");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::C), "C");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::Street), "STREET");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::Dc), "DC");
        ADD_IF_RECOGNIZED(enum_value(AttributeType::Uid), "UID");

        cert_name.appendff("\\{}={}", member_identifier, value);
    }
#undef ADD_IF_RECOGNIZED

    return cert_name.to_string();
}
}
