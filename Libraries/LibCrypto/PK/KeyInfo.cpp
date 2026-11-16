/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Memory.h>
#include <AK/ScopeGuard.h>
#include <LibCrypto/ASN1/Constants.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/PK/KeyInfo.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/x509.h>

namespace Crypto::PK {

static ErrorOr<ASN1::ObjectIdentifier> object_identifier(ASN1_OBJECT const* object)
{
    char text[256];
    auto length = OBJ_obj2txt(text, sizeof(text), object, 1);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(text))
        return Error::from_string_literal("Invalid object identifier");

    ASN1::ObjectIdentifier identifier;
    for (auto arc : StringView { text, static_cast<size_t>(length) }.split_view('.')) {
        auto value = arc.to_number<u32>();
        if (!value.has_value())
            return Error::from_string_literal("Invalid object identifier");
        TRY(identifier.try_append(*value));
    }
    return identifier;
}

static ErrorOr<AlgorithmIdentifier> algorithm_identifier(X509_ALGOR const* algorithm)
{
    ASN1_OBJECT const* object = nullptr;
    int parameters_type = 0;
    void const* parameters = nullptr;
    X509_ALGOR_get0(&object, &parameters_type, &parameters, algorithm);

    AlgorithmIdentifier result;
    result.identifier = TRY(object_identifier(object));
    if (parameters_type == V_ASN1_OBJECT)
        result.ec_parameters = TRY(object_identifier(static_cast<ASN1_OBJECT const*>(parameters)));
    return result;
}

static bool is_named_curve(AlgorithmIdentifier const& algorithm)
{
    if (!algorithm.ec_parameters.has_value())
        return false;
    auto const& curve = *algorithm.ec_parameters;
    return curve == ASN1::secp256r1_oid || curve == ASN1::secp384r1_oid || curve == ASN1::secp521r1_oid;
}

static bool is_ml_kem(ASN1::ObjectIdentifier const& identifier)
{
    return identifier == ASN1::ml_kem_512_oid || identifier == ASN1::ml_kem_768_oid || identifier == ASN1::ml_kem_1024_oid;
}

// NB: The structure is in DER, so its SEQUENCE's length takes one byte, or one more than the count in that byte's low
//     bits. Version 0 follows as 02 01 00.
static bool has_version_zero(ReadonlyBytes der)
{
    static constexpr u8 version_zero[] { 0x02, 0x01, 0x00 };
    if (der.size() < 2)
        return false;
    size_t header_size = 2 + ((der[1] & 0x80) ? (der[1] & 0x7f) : 0);
    return der.size() >= header_size + sizeof(version_zero) && der.slice(header_size, sizeof(version_zero)) == ReadonlyBytes { version_zero, sizeof(version_zero) };
}

static bool encodes_as(ReadonlyBytes expected, u8 const* encoded, int encoded_size)
{
    return encoded_size >= 0 && static_cast<size_t>(encoded_size) == expected.size() && expected == ReadonlyBytes { encoded, static_cast<size_t>(encoded_size) };
}

ErrorOr<SubjectPublicKeyInfo> parse_subject_public_key_info(ReadonlyBytes data, size_t& consumed)
{
    auto const* cursor = data.data();
    auto* public_key_info = d2i_X509_PUBKEY(nullptr, &cursor, data.size());
    if (!public_key_info)
        return Error::from_string_literal("Invalid SubjectPublicKeyInfo");
    ScopeGuard const free_public_key_info = [&] { X509_PUBKEY_free(public_key_info); };
    consumed = cursor - data.data();

    ASN1_OBJECT* object = nullptr;
    u8 const* key_bits = nullptr;
    int key_size = 0;
    X509_ALGOR* algorithm = nullptr;
    OPENSSL_TRY(X509_PUBKEY_get0_param(&object, &key_bits, &key_size, &algorithm, public_key_info));

    SubjectPublicKeyInfo result;
    result.algorithm = TRY(algorithm_identifier(algorithm));
    result.raw_key = TRY(ByteBuffer::copy(key_bits, key_size));

    // NB: OpenSSL runs the ML-KEM encapsulation key check when it decodes the key, but Web Crypto runs it at
    //     encapsulation. So an ML-KEM key is only checked to be in DER, without parameters.
    if (is_ml_kem(result.algorithm.identifier)) {
        int parameters_type = 0;
        X509_ALGOR_get0(nullptr, &parameters_type, nullptr, algorithm);
        if (parameters_type != V_ASN1_UNDEF)
            return Error::from_string_literal("The algorithm takes no parameters");

        u8* encoded = nullptr;
        auto encoded_size = i2d_X509_PUBKEY(public_key_info, &encoded);
        ScopeGuard const free_encoded = [&] { OPENSSL_free(encoded); };
        // NB: The BIT STRING's contents end the encoding, and are its count of unused bits followed by the key.
        if (!encodes_as(data.trim(consumed), encoded, encoded_size) || data[consumed - key_size - 1] != 0)
            return Error::from_string_literal("SubjectPublicKeyInfo is not in DER");
        return result;
    }

    auto* key = X509_PUBKEY_get(public_key_info);
    if (!key)
        return Error::from_string_literal("Invalid or unsupported public key");
    auto shared_key = OpenSSL_SharedPKEY { TRY(OpenSSL_PKEY::wrap(key)) };

    // NB: OpenSSL accepts BER, and parameters that an algorithm does not use. Its own encoding of the key is the DER
    //     encoding with the parameters that the algorithm takes, so any other input is not valid.
    u8* encoded = nullptr;
    auto encoded_size = i2d_PUBKEY(shared_key.ptr(), &encoded);
    ScopeGuard const free_encoded = [&] { OPENSSL_free(encoded); };
    if (!encodes_as(data.trim(consumed), encoded, encoded_size))
        return Error::from_string_literal("SubjectPublicKeyInfo is not in DER");

    auto const& identifier = result.algorithm.identifier;
    if (identifier == ASN1::rsa_encryption_oid) {
        result.rsa = RSAPublicKey::from_openssl_key(move(shared_key));
    } else if (identifier == ASN1::ec_public_key_encryption_oid) {
        if (!is_named_curve(result.algorithm))
            return Error::from_string_literal("Unsupported EC parameters");
        result.ec = TRY(ECPublicKey::from_octets(result.raw_key));
    }
    return result;
}

ErrorOr<PrivateKeyInfo> parse_private_key_info(ReadonlyBytes data, size_t& consumed)
{
    auto const* cursor = data.data();
    auto* private_key_info = d2i_PKCS8_PRIV_KEY_INFO(nullptr, &cursor, data.size());
    if (!private_key_info)
        return Error::from_string_literal("Invalid PrivateKeyInfo");
    ScopeGuard const free_private_key_info = [&] { PKCS8_PRIV_KEY_INFO_free(private_key_info); };
    consumed = cursor - data.data();

    {
        u8* encoded = nullptr;
        auto encoded_size = i2d_PKCS8_PRIV_KEY_INFO(private_key_info, &encoded);
        ScopeGuard const free_encoded = [&] { OPENSSL_clear_free(encoded, max(encoded_size, 0)); };
        if (!encodes_as(data.trim(consumed), encoded, encoded_size))
            return Error::from_string_literal("PrivateKeyInfo is not in DER");
    }

    // NB: OpenSSL also takes the version 1 OneAsymmetricKey of RFC 5958.
    if (!has_version_zero(data.trim(consumed)))
        return Error::from_string_literal("Unsupported PrivateKeyInfo version");

    ASN1_OBJECT const* object = nullptr;
    u8 const* private_key = nullptr;
    int private_key_size = 0;
    X509_ALGOR const* algorithm = nullptr;
    OPENSSL_TRY(PKCS8_pkey_get0(&object, &private_key, &private_key_size, &algorithm, private_key_info));

    // NB: The private key is copied into the result only once it has been checked, since a copy left in a result that
    //     is given up would not be cleared.
    ReadonlyBytes raw_key { private_key, static_cast<size_t>(private_key_size) };

    PrivateKeyInfo result;
    result.algorithm = TRY(algorithm_identifier(algorithm));

    auto const& identifier = result.algorithm.identifier;

    // https://www.rfc-editor.org/rfc/rfc8410#section-9
    // For all of the OIDs, the parameters MUST be absent.
    // https://www.rfc-editor.org/rfc/rfc9881#section-2
    // The contents of the parameters component for each algorithm MUST be absent.
    // NB: ML-KEM's parameters are absent likewise, and the outer encoding check above keeps whatever parameters were given.
    if (identifier != ASN1::rsa_encryption_oid && identifier != ASN1::ec_public_key_encryption_oid) {
        int parameters_type = 0;
        X509_ALGOR_get0(nullptr, &parameters_type, nullptr, algorithm);
        if (parameters_type != V_ASN1_UNDEF)
            return Error::from_string_literal("The algorithm takes no parameters");
    }

    // NB: The ML-DSA and ML-KEM privateKey choices are parsed here, since OpenSSL also takes forms that Web Crypto does
    //     not, and refuses an expandedKey that is not valid before Web Crypto can reject that form.
    if (identifier == ASN1::ml_dsa_44_oid || identifier == ASN1::ml_dsa_65_oid || identifier == ASN1::ml_dsa_87_oid) {
        auto size = identifier == ASN1::ml_dsa_44_oid ? MLDSA44 : identifier == ASN1::ml_dsa_65_oid ? MLDSA65
                                                                                                    : MLDSA87;
        result.mldsa = TRY(MLDSA::parse_mldsa_key(size, raw_key)).private_key;
        result.raw_key = TRY(ByteBuffer::copy(raw_key));
        return result;
    }
    if (is_ml_kem(identifier)) {
        auto size = identifier == ASN1::ml_kem_512_oid ? MLKEMSize::MLKEM512 : identifier == ASN1::ml_kem_768_oid ? MLKEMSize::MLKEM768
                                                                                                                  : MLKEMSize::MLKEM1024;
        result.mlkem = TRY(MLKEM::parse_mlkem_key(size, raw_key)).private_key;
        result.raw_key = TRY(ByteBuffer::copy(raw_key));
        return result;
    }

    auto* key = EVP_PKCS82PKEY(private_key_info);
    if (!key)
        return Error::from_string_literal("Invalid or unsupported private key");
    auto shared_key = OpenSSL_SharedPKEY { TRY(OpenSSL_PKEY::wrap(key)) };

    if (identifier == ASN1::rsa_encryption_oid) {
        // https://www.rfc-editor.org/rfc/rfc3279#section-2.3.1
        // The parameters field MUST have ASN.1 type NULL for this algorithm identifier.
        int parameters_type = 0;
        X509_ALGOR_get0(nullptr, &parameters_type, nullptr, algorithm);
        if (parameters_type != V_ASN1_NULL)
            return Error::from_string_literal("RSA parameters must be NULL");

        // NB: OpenSSL accepts BER in the RSAPrivateKey, so it must be the key's own DER encoding.
        u8* encoded = nullptr;
        auto encoded_size = i2d_PrivateKey(shared_key.ptr(), &encoded);
        ScopeGuard const free_encoded = [&] { OPENSSL_clear_free(encoded, max(encoded_size, 0)); };
        if (!encodes_as(raw_key, encoded, encoded_size))
            return Error::from_string_literal("RSAPrivateKey is not in DER");

        // NB: OpenSSL also takes other versions, and otherPrimeInfos even in version 0. Keys of more than two primes are
        //     not supported, as in JWK import.
        if (!has_version_zero(raw_key))
            return Error::from_string_literal("Unsupported RSAPrivateKey version");
        BIGNUM* third_prime = nullptr;
        bool has_third_prime = EVP_PKEY_get_bn_param(shared_key.ptr(), OSSL_PKEY_PARAM_RSA_FACTOR3, &third_prime) == 1;
        BN_clear_free(third_prime);
        if (has_third_prime)
            return Error::from_string_literal("RSA keys of more than two primes are not supported");

        result.rsa = RSAPrivateKey::from_openssl_key(move(shared_key));
    } else if (identifier == ASN1::ec_public_key_encryption_oid) {
        if (!is_named_curve(result.algorithm))
            return Error::from_string_literal("Unsupported EC parameters");
        result.ec = TRY(ECPrivateKey::from_private_key_info(shared_key.ptr(), *result.algorithm.ec_parameters, raw_key));
    }
    result.raw_key = TRY(ByteBuffer::copy(raw_key));
    return result;
}

ErrorOr<ReadonlyBytes> parse_octet_string(ReadonlyBytes data, size_t& consumed)
{
    auto const* cursor = data.data();
    auto* octet_string = d2i_ASN1_OCTET_STRING(nullptr, &cursor, data.size());
    if (!octet_string)
        return Error::from_string_literal("Invalid OCTET STRING");
    // NB: The OCTET STRING holds an OKP private key, so it is cleared.
    ScopeGuard const free_octet_string = [&] { ASN1_STRING_clear_free(octet_string); };
    consumed = cursor - data.data();

    u8* encoded = nullptr;
    auto encoded_size = i2d_ASN1_OCTET_STRING(octet_string, &encoded);
    ScopeGuard const free_encoded = [&] { OPENSSL_clear_free(encoded, max(encoded_size, 0)); };
    if (!encodes_as(data.trim(consumed), encoded, encoded_size))
        return Error::from_string_literal("OCTET STRING is not in DER");

    // NB: In DER the contents are the last bytes of the encoding.
    return data.slice(consumed - ASN1_STRING_length(octet_string), ASN1_STRING_length(octet_string));
}

}
