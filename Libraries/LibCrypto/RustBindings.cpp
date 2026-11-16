/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/Memory.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/Utf16String.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/ASN1/Constants.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Authentication/HMAC.h>
#include <LibCrypto/Authentication/KMAC.h>
#include <LibCrypto/Cipher/AES.h>
#include <LibCrypto/Cipher/ChaCha.h>
#include <LibCrypto/Curves/EdwardsCurve.h>
#include <LibCrypto/Hash/Argon2.h>
#include <LibCrypto/Hash/HKDF.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibCrypto/Hash/PBKDF2.h>
#include <LibCrypto/Hash/SHAKE.h>
#include <LibCrypto/KeyHandle.h>
#include <LibCrypto/PK/EC.h>
#include <LibCrypto/PK/KeyInfo.h>
#include <LibCrypto/PK/MLDSA.h>
#include <LibCrypto/PK/MLKEM.h>
#include <LibCrypto/PK/PK.h>
#include <LibCrypto/PK/RSA.h>

namespace Crypto {

template<typename T>
struct Asn1Storage final {
    AK_ALLOC_WITH_KMALLOC;

    T value;
};

using SubjectPublicKeyStorage = Asn1Storage<PK::SubjectPublicKeyInfo>;
using PrivateKeyStorage = Asn1Storage<PK::PrivateKeyInfo>;
using OctetStringStorage = Asn1Storage<ReadonlyBytes>;

extern "C" {
using ReceiveEncodedBytes = void (*)(void*, ReadonlyBytes);
enum class Asn1Structure : u8 {
    SubjectPublicKeyInfo,
    PrivateKeyInfo,
    OctetString,
};
FlatPtr crypto_decode_asn1_structure(ReadonlyBytes, Asn1Structure, void*, bool*);
}

static bool receive_byte_result(ErrorOr<ByteBuffer> const& result, void* state, ReceiveEncodedBytes receive)
{
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

FlatPtr crypto_decode_asn1_structure(ReadonlyBytes data, Asn1Structure structure, void* out, bool* all_consumed)
{
    size_t consumed = 0;
    switch (structure) {
    case Asn1Structure::SubjectPublicKeyInfo: {
        auto subject_public_key = ::Crypto::PK::parse_subject_public_key_info(data, consumed);
        if (subject_public_key.is_error())
            return Utf16String::formatted("{}", subject_public_key.release_error()).into_raw();
        static_cast<SubjectPublicKeyStorage*>(out)->value = subject_public_key.release_value();
        break;
    }
    case Asn1Structure::PrivateKeyInfo: {
        auto private_key = ::Crypto::PK::parse_private_key_info(data, consumed);
        if (private_key.is_error())
            return Utf16String::formatted("{}", private_key.release_error()).into_raw();
        static_cast<PrivateKeyStorage*>(out)->value = private_key.release_value();
        break;
    }
    case Asn1Structure::OctetString: {
        auto octet_string = ::Crypto::PK::parse_octet_string(data, consumed);
        if (octet_string.is_error())
            return Utf16String::formatted("{}", octet_string.error()).into_raw();
        static_cast<OctetStringStorage*>(out)->value = octet_string.release_value();
        break;
    }
    }
    *all_consumed = consumed == data.size();
    return 0;
}

extern "C" {
void* crypto_asn1_allocate_key_info(Asn1Structure);
void crypto_asn1_delete_key_info(Asn1Structure, void*);
bool crypto_rsa_key_info_has_encryption_oid(Asn1Structure, void const*);
KeyHandle* crypto_rsa_key_info_read_key(Asn1Structure, void const*);
bool crypto_rsa_key_is_valid(KeyHandle const*, bool*);
bool crypto_rsa_validate_modulus_length(u32);
bool crypto_rsa_generate_key_pair(u32, ReadonlyBytes, KeyHandle**, KeyHandle**);
bool crypto_rsa_read_key_metadata(KeyHandle const*, u32*, void*, ReceiveEncodedBytes);
KeyHandle* crypto_rsa_key_from_jwk_components(bool, ReadonlySpan<ReadonlyBytes>);
bool crypto_rsa_export_der(KeyHandle const*, bool, void*, ReceiveEncodedBytes);
bool crypto_rsa_export_jwk_integer(KeyHandle const*, u8, void*, ReceiveEncodedBytes);
bool crypto_rsa_oaep_cipher(bool, ::Crypto::Hash::HashKind, KeyHandle const*, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool crypto_rsa_pss_sign(::Crypto::Hash::HashKind, KeyHandle const*, u32, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool crypto_rsa_pss_verify(::Crypto::Hash::HashKind, KeyHandle const*, u32, ReadonlyBytes, ReadonlyBytes, bool*);
bool crypto_rsa_pkcs1_sign(::Crypto::Hash::HashKind, KeyHandle const*, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool crypto_rsa_pkcs1_verify(::Crypto::Hash::HashKind, KeyHandle const*, ReadonlyBytes, ReadonlyBytes, bool*);
}

void* crypto_asn1_allocate_key_info(Asn1Structure structure)
{
    if (structure == Asn1Structure::SubjectPublicKeyInfo)
        return new SubjectPublicKeyStorage;
    if (structure == Asn1Structure::OctetString)
        return new OctetStringStorage;
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    return new PrivateKeyStorage;
}

void crypto_asn1_delete_key_info(Asn1Structure structure, void* key_info)
{
    if (structure == Asn1Structure::SubjectPublicKeyInfo) {
        delete static_cast<SubjectPublicKeyStorage*>(key_info);
        return;
    }
    if (structure == Asn1Structure::OctetString) {
        delete static_cast<OctetStringStorage*>(key_info);
        return;
    }
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    auto* storage = static_cast<PrivateKeyStorage*>(key_info);
    secure_zero(storage->value.raw_key.data(), storage->value.raw_key.size());
    delete storage;
}

bool crypto_rsa_key_info_has_encryption_oid(Asn1Structure structure, void const* key_info)
{
    if (structure == Asn1Structure::SubjectPublicKeyInfo)
        return static_cast<SubjectPublicKeyStorage const*>(key_info)->value.algorithm.identifier == ::Crypto::ASN1::rsa_encryption_oid;
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    return static_cast<PrivateKeyStorage const*>(key_info)->value.algorithm.identifier == ::Crypto::ASN1::rsa_encryption_oid;
}

KeyHandle* crypto_rsa_key_info_read_key(Asn1Structure structure, void const* key_info)
{
    if (structure == Asn1Structure::SubjectPublicKeyInfo)
        return new KeyHandle(static_cast<SubjectPublicKeyStorage const*>(key_info)->value.rsa);
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    return new KeyHandle(static_cast<PrivateKeyStorage const*>(key_info)->value.rsa);
}

bool crypto_rsa_key_is_valid(KeyHandle const* key, bool* valid)
{
    auto result = key->visit(
        [](::Crypto::PK::RSAPublicKey const& public_key) { return public_key.is_valid(); },
        [](::Crypto::PK::RSAPrivateKey const& private_key) { return private_key.is_valid(); },
        [](auto const&) -> ErrorOr<bool> { VERIFY_NOT_REACHED(); });
    if (result.is_error())
        return false;
    *valid = result.value();
    return true;
}

bool crypto_rsa_validate_modulus_length(u32 modulus_length)
{
    return !::Crypto::PK::RSA::validate_key_generation_parameters(modulus_length).is_error();
}

bool crypto_rsa_generate_key_pair(u32 modulus_length, ReadonlyBytes public_exponent, KeyHandle** out_public_key, KeyHandle** out_private_key)
{
    auto key_pair = ::Crypto::PK::RSA::generate_key_pair(modulus_length, public_exponent);
    if (key_pair.is_error())
        return false;
    *out_public_key = new KeyHandle(move(key_pair.value().public_key));
    *out_private_key = new KeyHandle(move(key_pair.value().private_key));
    return true;
}

bool crypto_rsa_read_key_metadata(KeyHandle const* key, u32* modulus_length, void* state, ReceiveEncodedBytes receive)
{
    auto result = key->visit(
        [&](::Crypto::PK::RSAPublicKey const& public_key) {
            *modulus_length = public_key.modulus_bits();
            return public_key.public_exponent();
        },
        [&](::Crypto::PK::RSAPrivateKey const& private_key) {
            *modulus_length = private_key.modulus_bits();
            return private_key.component(::Crypto::PK::RSAPrivateKey::Component::PublicExponent);
        },
        [](auto const&) -> ErrorOr<ByteBuffer> { VERIFY_NOT_REACHED(); });
    return receive_byte_result(result, state, receive);
}

KeyHandle* crypto_rsa_key_from_jwk_components(bool is_private, ReadonlySpan<ReadonlyBytes> components)
{
    auto key = [&]() -> ErrorOr<KeyHandle> {
        if (!is_private)
            return TRY(::Crypto::PK::RSAPublicKey::from_components(components[0], components[1]));

        // NB: A JWK of a key without CRT components may carry them empty.
        auto has_crt_components = components.size() == 8 && any_of(components.slice(3), [](auto const& component) { return !component.is_empty(); });
        Optional<::Crypto::PK::RSACRTComponents> crt_components;
        if (has_crt_components)
            crt_components = ::Crypto::PK::RSACRTComponents { components[3], components[4], components[5], components[6], components[7] };
        return TRY(::Crypto::PK::RSAPrivateKey::from_components(components[0], components[1], components[2], crt_components));
    }();
    if (key.is_error())
        return nullptr;
    return new KeyHandle(key.release_value());
}

bool crypto_rsa_export_der(KeyHandle const* key, bool is_public, void* state, ReceiveEncodedBytes receive)
{
    auto result = [&]() -> ErrorOr<ByteBuffer> {
        if (is_public) {
            auto public_key = TRY(key->get<::Crypto::PK::RSAPublicKey>().export_as_der());
            return ::Crypto::PK::wrap_in_subject_public_key_info(public_key, ::Crypto::ASN1::rsa_encryption_oid, nullptr);
        }
        auto private_key = TRY(key->get<::Crypto::PK::RSAPrivateKey>().export_as_der());
        ScopeGuard const clear_private_key = [&] { secure_zero(private_key.data(), private_key.size()); };
        return ::Crypto::PK::wrap_in_private_key_info(private_key, ::Crypto::ASN1::rsa_encryption_oid, nullptr);
    }();
    return receive_byte_result(result, state, receive);
}

bool crypto_rsa_export_jwk_integer(KeyHandle const* key, u8 component, void* state, ReceiveEncodedBytes receive)
{
    auto integer = key->visit(
        [&](::Crypto::PK::RSAPublicKey const& public_key) {
            VERIFY(component < 2);
            return component == 0 ? public_key.modulus() : public_key.public_exponent();
        },
        [&](::Crypto::PK::RSAPrivateKey const& private_key) {
            VERIFY(component <= to_underlying(::Crypto::PK::RSAPrivateKey::Component::Coefficient));
            return private_key.component(static_cast<::Crypto::PK::RSAPrivateKey::Component>(component));
        },
        [](auto const&) -> ErrorOr<ByteBuffer> { VERIFY_NOT_REACHED(); });
    if (integer.is_error())
        return false;
    receive(state, integer.value());
    secure_zero(integer.value().data(), integer.value().size());
    return true;
}

bool crypto_rsa_oaep_cipher(bool encrypt, ::Crypto::Hash::HashKind hash_kind, KeyHandle const* key, ReadonlyBytes label, ReadonlyBytes input, void* state, ReceiveEncodedBytes receive)
{
    auto result = encrypt
        ? ::Crypto::PK::rsa_oaep_encrypt(key->get<::Crypto::PK::RSAPublicKey>(), hash_kind, label, input)
        : ::Crypto::PK::rsa_oaep_decrypt(key->get<::Crypto::PK::RSAPrivateKey>(), hash_kind, label, input);
    if (result.is_error())
        return false;
    receive(state, result.value());
    if (!encrypt)
        secure_zero(result.value().data(), result.value().size());
    return true;
}

bool crypto_rsa_pss_sign(::Crypto::Hash::HashKind hash_kind, KeyHandle const* key, u32 salt_length, ReadonlyBytes message, void* state, ReceiveEncodedBytes receive)
{
    auto result = ::Crypto::PK::rsa_pss_sign(key->get<::Crypto::PK::RSAPrivateKey>(), hash_kind, salt_length, message);
    return receive_byte_result(result, state, receive);
}

bool crypto_rsa_pss_verify(::Crypto::Hash::HashKind hash_kind, KeyHandle const* key, u32 salt_length, ReadonlyBytes message, ReadonlyBytes signature, bool* verified)
{
    auto result = ::Crypto::PK::rsa_pss_verify(key->get<::Crypto::PK::RSAPublicKey>(), hash_kind, salt_length, message, signature);
    if (result.is_error())
        return false;
    *verified = result.value();
    return true;
}

bool crypto_rsa_pkcs1_sign(::Crypto::Hash::HashKind hash_kind, KeyHandle const* key, ReadonlyBytes message, void* state, ReceiveEncodedBytes receive)
{
    auto result = ::Crypto::PK::rsa_pkcs1_sign(key->get<::Crypto::PK::RSAPrivateKey>(), hash_kind, message);
    return receive_byte_result(result, state, receive);
}

bool crypto_rsa_pkcs1_verify(::Crypto::Hash::HashKind hash_kind, KeyHandle const* key, ReadonlyBytes message, ReadonlyBytes signature, bool* verified)
{
    auto result = ::Crypto::PK::rsa_pkcs1_verify(key->get<::Crypto::PK::RSAPublicKey>(), hash_kind, message, signature);
    if (result.is_error())
        return false;
    *verified = result.value();
    return true;
}

extern "C" {
bool crypto_aes_cbc_cipher(bool, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool crypto_aes_ctr_cipher(bool, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool crypto_aes_kw_cipher(bool, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool crypto_aes_gcm_encrypt(ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, size_t, void*, void*, ReceiveEncodedBytes);
bool crypto_aes_gcm_decrypt(ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
FlatPtr crypto_aes_ocb_cipher(bool, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, size_t, void*, ReceiveEncodedBytes);
}

bool crypto_aes_cbc_cipher(bool encrypt, ReadonlyBytes key, ReadonlyBytes input, ReadonlyBytes iv, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Cipher::AESCBCCipher cipher(key);
    auto result = encrypt ? cipher.encrypt(input, iv) : cipher.decrypt(input, iv);
    return receive_byte_result(result, state, receive);
}

bool crypto_aes_ctr_cipher(bool encrypt, ReadonlyBytes key, ReadonlyBytes input, ReadonlyBytes counter, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Cipher::AESCTRCipher cipher(key);
    auto result = encrypt ? cipher.encrypt(input, counter) : cipher.decrypt(input, counter);
    return receive_byte_result(result, state, receive);
}

bool crypto_aes_kw_cipher(bool wrap, ReadonlyBytes key, ReadonlyBytes input, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Cipher::AESKWCipher cipher(key);
    auto result = wrap ? cipher.wrap(input) : cipher.unwrap(input);
    return receive_byte_result(result, state, receive);
}

bool crypto_aes_gcm_encrypt(ReadonlyBytes key, ReadonlyBytes plaintext, ReadonlyBytes iv, ReadonlyBytes additional_data, size_t tag_length, void* ciphertext, void* tag, ReceiveEncodedBytes receive)
{
    ::Crypto::Cipher::AESGCMCipher cipher(key);
    auto result = cipher.encrypt(plaintext, iv, additional_data, tag_length);
    if (result.is_error())
        return false;
    receive(ciphertext, result.value().ciphertext);
    receive(tag, result.value().tag);
    return true;
}

bool crypto_aes_gcm_decrypt(ReadonlyBytes key, ReadonlyBytes ciphertext, ReadonlyBytes iv, ReadonlyBytes additional_data, ReadonlyBytes tag, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Cipher::AESGCMCipher cipher(key);
    auto result = cipher.decrypt(ciphertext, iv, additional_data, tag);
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

FlatPtr crypto_aes_ocb_cipher(bool encrypt, ReadonlyBytes key, ReadonlyBytes input, ReadonlyBytes iv, ReadonlyBytes additional_data, size_t tag_length, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Cipher::AESOCBCipher cipher(key);
    auto result = encrypt ? cipher.encrypt(input, iv, additional_data, tag_length) : cipher.decrypt(input, iv, additional_data, tag_length);
    if (result.is_error())
        return Utf16String::formatted("{}", result.error()).into_raw();
    receive(state, result.value());
    return 0;
}

static_assert(to_underlying(::Crypto::Hash::HashKind::SHA1) == 2);
static_assert(to_underlying(::Crypto::Hash::HashKind::SHA256) == 3);
static_assert(to_underlying(::Crypto::Hash::HashKind::SHA384) == 4);
static_assert(to_underlying(::Crypto::Hash::HashKind::SHA512) == 5);
static_assert(to_underlying(::Crypto::Hash::HashKind::SHA3_256) == 6);
static_assert(to_underlying(::Crypto::Hash::HashKind::SHA3_384) == 7);
static_assert(to_underlying(::Crypto::Hash::HashKind::SHA3_512) == 8);

extern "C" bool crypto_sha_digest(::Crypto::Hash::HashKind, ReadonlyBytes, void*, ReceiveEncodedBytes);

bool crypto_sha_digest(::Crypto::Hash::HashKind hash_kind, ReadonlyBytes data, void* state, ReceiveEncodedBytes receive)
{
    auto digest = ::Crypto::Hash::digest(hash_kind, data);
    receive(state, digest.bytes());
    return true;
}

struct OptionalBytes {
    bool has_value;
    ReadonlyBytes bytes;

    Optional<ReadonlyBytes> to_optional() const { return has_value ? Optional<ReadonlyBytes> { bytes } : Optional<ReadonlyBytes> {}; }
};

enum class EcCurve : u8 {
    P256,
    P384,
    P521,
};

static size_t ec_scalar_size(EcCurve curve)
{
    switch (curve) {
    case EcCurve::P256:
        return 32;
    case EcCurve::P384:
        return 48;
    case EcCurve::P521:
        return 66;
    }
    VERIFY_NOT_REACHED();
}

static ReadonlySpan<u32> ec_curve_oid(EcCurve curve)
{
    switch (curve) {
    case EcCurve::P256:
        return ::Crypto::ASN1::secp256r1_oid;
    case EcCurve::P384:
        return ::Crypto::ASN1::secp384r1_oid;
    case EcCurve::P521:
        return ::Crypto::ASN1::secp521r1_oid;
    }
    VERIFY_NOT_REACHED();
}

extern "C" {
bool crypto_ec_generate_key_pair(EcCurve, KeyHandle**, KeyHandle**);
FlatPtr crypto_ec_sign(EcCurve, KeyHandle const*, ReadonlyBytes, void*, void*, ReceiveEncodedBytes);
FlatPtr crypto_ec_verify(EcCurve, KeyHandle const*, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, bool*);
size_t crypto_ec_public_key_scalar_size(KeyHandle const*);
FlatPtr crypto_ec_compute_coordinate(EcCurve, KeyHandle const*, KeyHandle const*, void*, ReceiveEncodedBytes);
bool crypto_ec_key_info_has_public_key_oid(Asn1Structure, void const*);
bool crypto_ec_key_info_parameters(Asn1Structure, void const*, ReadonlySpan<u32>*);
bool crypto_ec_private_key_info_parameters_match(void const*);
KeyHandle* crypto_ec_key_info_read_key(Asn1Structure, void const*);
KeyHandle* crypto_ec_key_from_coordinates(ReadonlyBytes, ReadonlyBytes, OptionalBytes);
KeyHandle* crypto_ec_parse_raw_public_key(ReadonlyBytes);
bool crypto_ec_key_is_valid(EcCurve, KeyHandle const*, bool*);
bool crypto_ec_export_der(EcCurve, KeyHandle const*, bool, void*, ReceiveEncodedBytes);
bool crypto_ec_export_uncompressed(KeyHandle const*, void*, ReceiveEncodedBytes);
bool crypto_ec_export_public_coordinates(KeyHandle const*, void*, void*, ReceiveEncodedBytes);
bool crypto_ec_export_private_scalar(KeyHandle const*, void*, ReceiveEncodedBytes);
}

bool crypto_ec_generate_key_pair(EcCurve curve, KeyHandle** out_public_key, KeyHandle** out_private_key)
{
    auto private_key = ::Crypto::PK::ECPrivateKey::generate(ec_scalar_size(curve));
    if (private_key.is_error())
        return false;
    *out_public_key = new KeyHandle(*private_key.value().public_key());
    *out_private_key = new KeyHandle(private_key.release_value());
    return true;
}

FlatPtr crypto_ec_sign(EcCurve curve, KeyHandle const* key, ReadonlyBytes digest, void* r, void* s, ReceiveEncodedBytes receive)
{
    auto const& private_key = key->get<::Crypto::PK::ECPrivateKey>();
    if (private_key.scalar_size() != ec_scalar_size(curve))
        return Utf16String::from_utf8("The key is on a different curve"sv).into_raw();
    auto signature = ::Crypto::PK::ecdsa_sign(private_key, digest);
    if (signature.is_error())
        return Utf16String::formatted("{}", signature.error()).into_raw();
    receive(r, signature.value().r);
    receive(s, signature.value().s);
    return 0;
}

FlatPtr crypto_ec_verify(EcCurve curve, KeyHandle const* key, ReadonlyBytes digest, ReadonlyBytes r, ReadonlyBytes s, bool* verified)
{
    auto const& public_key = key->get<::Crypto::PK::ECPublicKey>();
    if (public_key.scalar_size() != ec_scalar_size(curve)) {
        *verified = false;
        return 0;
    }
    auto result = ::Crypto::PK::ecdsa_verify(public_key, digest, r, s);
    if (result.is_error())
        return Utf16String::from_utf8(result.error().string_literal()).into_raw();
    *verified = result.value();
    return 0;
}

size_t crypto_ec_public_key_scalar_size(KeyHandle const* key)
{
    return key->get<::Crypto::PK::ECPublicKey>().scalar_size();
}

FlatPtr crypto_ec_compute_coordinate(EcCurve curve, KeyHandle const* key, KeyHandle const* public_key, void* state, ReceiveEncodedBytes receive)
{
    auto const& private_key = key->get<::Crypto::PK::ECPrivateKey>();
    if (private_key.scalar_size() != ec_scalar_size(curve))
        return Utf16String::from_utf8("The key is on a different curve"sv).into_raw();
    auto secret = ::Crypto::PK::ecdh_derive(private_key, public_key->get<::Crypto::PK::ECPublicKey>());
    if (secret.is_error())
        return Utf16String::formatted("{}", secret.error()).into_raw();
    receive(state, secret.value());
    secure_zero(secret.value().data(), secret.value().size());
    return 0;
}

static ::Crypto::PK::AlgorithmIdentifier const& key_info_algorithm(Asn1Structure structure, void const* key_info)
{
    if (structure == Asn1Structure::SubjectPublicKeyInfo)
        return static_cast<SubjectPublicKeyStorage const*>(key_info)->value.algorithm;
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    return static_cast<PrivateKeyStorage const*>(key_info)->value.algorithm;
}

bool crypto_ec_key_info_has_public_key_oid(Asn1Structure structure, void const* key_info)
{
    return key_info_algorithm(structure, key_info).identifier == ::Crypto::ASN1::ec_public_key_encryption_oid;
}

bool crypto_ec_key_info_parameters(Asn1Structure structure, void const* key_info, ReadonlySpan<u32>* parameters)
{
    auto const& algorithm = key_info_algorithm(structure, key_info);
    if (!algorithm.ec_parameters.has_value())
        return false;
    *parameters = algorithm.ec_parameters->span();
    return true;
}

bool crypto_ec_private_key_info_parameters_match(void const* key_info)
{
    auto const& private_key_info = static_cast<PrivateKeyStorage const*>(key_info)->value;
    auto const& parameters = private_key_info.ec.parameters();
    return !parameters.has_value() || *parameters == private_key_info.algorithm.ec_parameters.value_or({});
}

KeyHandle* crypto_ec_key_info_read_key(Asn1Structure structure, void const* key_info)
{
    if (structure == Asn1Structure::SubjectPublicKeyInfo)
        return new KeyHandle(static_cast<SubjectPublicKeyStorage const*>(key_info)->value.ec);
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    return new KeyHandle(static_cast<PrivateKeyStorage const*>(key_info)->value.ec);
}

KeyHandle* crypto_ec_key_from_coordinates(ReadonlyBytes x, ReadonlyBytes y, OptionalBytes d)
{
    auto public_key = ::Crypto::PK::ECPublicKey::from_coordinates(x, y);
    if (public_key.is_error())
        return nullptr;
    if (!d.has_value)
        return new KeyHandle(public_key.release_value());
    auto private_key = ::Crypto::PK::ECPrivateKey::from_scalar(d.bytes, {}, public_key.release_value());
    if (private_key.is_error())
        return nullptr;
    return new KeyHandle(private_key.release_value());
}

KeyHandle* crypto_ec_parse_raw_public_key(ReadonlyBytes data)
{
    auto public_key = ::Crypto::PK::ECPublicKey::from_octets(data);
    if (public_key.is_error())
        return nullptr;
    return new KeyHandle(public_key.release_value());
}

// NB: Every EC key is a valid key for its own curve, since building it checks that.
bool crypto_ec_key_is_valid(EcCurve curve, KeyHandle const* key, bool* valid)
{
    auto scalar_size = key->visit(
        [](::Crypto::PK::ECPublicKey const& public_key) { return public_key.scalar_size(); },
        [](::Crypto::PK::ECPrivateKey const& private_key) { return private_key.scalar_size(); },
        [](auto const&) -> size_t { VERIFY_NOT_REACHED(); });
    *valid = scalar_size == ec_scalar_size(curve);
    return true;
}

bool crypto_ec_export_der(EcCurve curve, KeyHandle const* key, bool is_public, void* state, ReceiveEncodedBytes receive)
{
    auto result = [&]() -> ErrorOr<ByteBuffer> {
        auto ec_params = ec_curve_oid(curve);
        if (is_public) {
            auto public_key_bytes = TRY(key->get<::Crypto::PK::ECPublicKey>().to_uncompressed());
            return ::Crypto::PK::wrap_in_subject_public_key_info(public_key_bytes, ::Crypto::ASN1::ec_public_key_encryption_oid, ec_params);
        }
        auto private_key = TRY(key->get<::Crypto::PK::ECPrivateKey>().export_as_der());
        ScopeGuard const clear_private_key = [&] { secure_zero(private_key.data(), private_key.size()); };
        return ::Crypto::PK::wrap_in_private_key_info(private_key, ::Crypto::ASN1::ec_public_key_encryption_oid, ec_params);
    }();
    return receive_byte_result(result, state, receive);
}

bool crypto_ec_export_uncompressed(KeyHandle const* key, void* state, ReceiveEncodedBytes receive)
{
    auto result = key->get<::Crypto::PK::ECPublicKey>().to_uncompressed();
    return receive_byte_result(result, state, receive);
}

bool crypto_ec_export_public_coordinates(KeyHandle const* key, void* x, void* y, ReceiveEncodedBytes receive)
{
    auto result = [&]() -> ErrorOr<void> {
        auto public_key = TRY(key->visit(
            [](::Crypto::PK::ECPublicKey const& public_key) -> ErrorOr<::Crypto::PK::ECPublicKey> { return public_key; },
            [](::Crypto::PK::ECPrivateKey const& private_key) { return private_key.derive_public_key(); },
            [](auto const&) -> ErrorOr<::Crypto::PK::ECPublicKey> { VERIFY_NOT_REACHED(); }));
        auto x_bytes = TRY(public_key.x_bytes());
        auto y_bytes = TRY(public_key.y_bytes());
        receive(x, x_bytes);
        receive(y, y_bytes);
        return {};
    }();
    return !result.is_error();
}

bool crypto_ec_export_private_scalar(KeyHandle const* key, void* state, ReceiveEncodedBytes receive)
{
    auto result = key->get<::Crypto::PK::ECPrivateKey>().d_bytes();
    if (result.is_error())
        return false;
    receive(state, result.value());
    secure_zero(result.value().data(), result.value().size());
    return true;
}

enum class OkpCurve : u8 {
    Ed25519,
    Ed448,
    X25519,
    X448,
};

static ::Crypto::Curves::EdwardsCurve okp_curve(OkpCurve curve)
{
    switch (curve) {
    case OkpCurve::Ed25519:
        return ::Crypto::Curves::Ed25519 {};
    case OkpCurve::Ed448:
        return ::Crypto::Curves::Ed448 {};
    case OkpCurve::X25519:
        return ::Crypto::Curves::X25519 {};
    case OkpCurve::X448:
        return ::Crypto::Curves::X448 {};
    }
    VERIFY_NOT_REACHED();
}

static ReadonlySpan<u32> okp_curve_oid(OkpCurve curve)
{
    switch (curve) {
    case OkpCurve::Ed25519:
        return ::Crypto::ASN1::ed25519_oid;
    case OkpCurve::Ed448:
        return ::Crypto::ASN1::ed448_oid;
    case OkpCurve::X25519:
        return ::Crypto::ASN1::x25519_oid;
    case OkpCurve::X448:
        return ::Crypto::ASN1::x448_oid;
    }
    VERIFY_NOT_REACHED();
}

extern "C" {
bool crypto_okp_generate_private_key(OkpCurve, void*, ReceiveEncodedBytes);
bool crypto_okp_generate_public_key(OkpCurve, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool crypto_okp_compute_coordinate(OkpCurve, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool crypto_okp_sign(OkpCurve, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
FlatPtr crypto_okp_verify(OkpCurve, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, bool*);
bool crypto_okp_key_info_has_oid(OkpCurve, Asn1Structure, void const*);
void crypto_asn1_key_info_raw_key(Asn1Structure, void const*, ReadonlyBytes*);
void crypto_asn1_octet_string_bytes(OctetStringStorage const*, ReadonlyBytes*);
bool crypto_okp_export_der(OkpCurve, bool, ReadonlyBytes, void*, ReceiveEncodedBytes);
}

bool crypto_okp_generate_private_key(OkpCurve curve, void* state, ReceiveEncodedBytes receive)
{
    auto result = okp_curve(curve).generate_private_key();
    return receive_byte_result(result, state, receive);
}

bool crypto_okp_generate_public_key(OkpCurve curve, ReadonlyBytes private_key, void* state, ReceiveEncodedBytes receive)
{
    auto result = okp_curve(curve).generate_public_key(private_key);
    return receive_byte_result(result, state, receive);
}

bool crypto_okp_compute_coordinate(OkpCurve curve, ReadonlyBytes private_key, ReadonlyBytes public_key, void* state, ReceiveEncodedBytes receive)
{
    VERIFY(curve == OkpCurve::X25519 || curve == OkpCurve::X448);
    auto result = curve == OkpCurve::X25519
        ? ::Crypto::Curves::X25519 {}.compute_coordinate(private_key, public_key)
        : ::Crypto::Curves::X448 {}.compute_coordinate(private_key, public_key);
    return receive_byte_result(result, state, receive);
}

bool crypto_okp_sign(OkpCurve curve, ReadonlyBytes private_key, ReadonlyBytes message, ReadonlyBytes context, void* state, ReceiveEncodedBytes receive)
{
    VERIFY(curve == OkpCurve::Ed25519 || curve == OkpCurve::Ed448);
    auto result = curve == OkpCurve::Ed25519
        ? ::Crypto::Curves::Ed25519 {}.sign(private_key, message)
        : ::Crypto::Curves::Ed448 {}.sign(private_key, message, context);
    return receive_byte_result(result, state, receive);
}

FlatPtr crypto_okp_verify(OkpCurve curve, ReadonlyBytes public_key, ReadonlyBytes signature, ReadonlyBytes message, ReadonlyBytes context, bool* verified)
{
    VERIFY(curve == OkpCurve::Ed25519 || curve == OkpCurve::Ed448);
    auto result = curve == OkpCurve::Ed25519
        ? ::Crypto::Curves::Ed25519 {}.verify(public_key, signature, message)
        : ::Crypto::Curves::Ed448 {}.verify(public_key, signature, message, context);
    if (result.is_error())
        return Utf16String::from_utf8(result.error().string_literal()).into_raw();
    *verified = result.value();
    return 0;
}

bool crypto_okp_key_info_has_oid(OkpCurve curve, Asn1Structure structure, void const* key_info)
{
    return key_info_algorithm(structure, key_info).identifier == okp_curve_oid(curve);
}

void crypto_asn1_key_info_raw_key(Asn1Structure structure, void const* key_info, ReadonlyBytes* bytes)
{
    if (structure == Asn1Structure::SubjectPublicKeyInfo) {
        *bytes = static_cast<SubjectPublicKeyStorage const*>(key_info)->value.raw_key;
        return;
    }
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    *bytes = static_cast<PrivateKeyStorage const*>(key_info)->value.raw_key;
}

void crypto_asn1_octet_string_bytes(OctetStringStorage const* string, ReadonlyBytes* bytes)
{
    *bytes = string->value;
}

bool crypto_okp_export_der(OkpCurve curve, bool is_public, ReadonlyBytes key, void* state, ReceiveEncodedBytes receive)
{
    auto result = [&]() -> ErrorOr<ByteBuffer> {
        auto oid = okp_curve_oid(curve);
        if (is_public)
            return ::Crypto::PK::wrap_in_subject_public_key_info(key, oid);
        ::Crypto::ASN1::Encoder encoder;
        TRY(encoder.write(key));
        auto private_key = encoder.finish();
        ScopeGuard const clear_private_key = [&] { secure_zero(private_key.data(), private_key.size()); };
        return ::Crypto::PK::wrap_in_private_key_info(private_key, oid);
    }();
    return receive_byte_result(result, state, receive);
}

extern "C" {
bool crypto_hmac_sign(::Crypto::Hash::HashKind, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
}

bool crypto_hmac_sign(::Crypto::Hash::HashKind hash_kind, ReadonlyBytes key, ReadonlyBytes message, void* state, ReceiveEncodedBytes receive)
{
    auto mac = ::Crypto::Authentication::hmac(hash_kind, key, message);
    if (mac.is_error())
        return false;
    receive(state, mac.value().bytes());
    return true;
}

extern "C" {
bool crypto_mldsa_sign(::Crypto::PK::MLDSASize, KeyHandle const*, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool crypto_mldsa_verify(::Crypto::PK::MLDSASize, KeyHandle const*, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, bool*);
FlatPtr crypto_mldsa_generate_key_pair(::Crypto::PK::MLDSASize, ReadonlyBytes, KeyHandle**, KeyHandle**);
void crypto_asn1_key_info_identifier(Asn1Structure, void const*, ReadonlySpan<u32>*);
KeyHandle* crypto_mldsa_key_info_read_private_key(void const*);
KeyHandle* crypto_mldsa_read_public_key(::Crypto::PK::MLDSASize, ReadonlyBytes);
void crypto_mldsa_public_bytes(KeyHandle const*, ReadonlyBytes*);
bool crypto_mldsa_seed_bytes(KeyHandle const*, void*, ReceiveEncodedBytes);
bool crypto_mldsa_export_der(KeyHandle const*, bool, ReadonlySpan<u32>, void*, ReceiveEncodedBytes);
}

static_assert(to_underlying(::Crypto::PK::MLDSA44) == 0);
static_assert(to_underlying(::Crypto::PK::MLDSA65) == 1);
static_assert(to_underlying(::Crypto::PK::MLDSA87) == 2);

bool crypto_mldsa_sign(::Crypto::PK::MLDSASize size, KeyHandle const* key, ReadonlyBytes message, ReadonlyBytes context, void* state, ReceiveEncodedBytes receive)
{
    auto result = ::Crypto::PK::mldsa_sign(size, key->get<::Crypto::PK::MLDSAPrivateKey>(), context, message);
    return receive_byte_result(result, state, receive);
}

bool crypto_mldsa_verify(::Crypto::PK::MLDSASize size, KeyHandle const* key, ReadonlyBytes message, ReadonlyBytes signature, ReadonlyBytes context, bool* verified)
{
    auto result = ::Crypto::PK::mldsa_verify(size, key->get<::Crypto::PK::MLDSAPublicKey>(), context, message, signature);
    if (result.is_error())
        return false;
    *verified = result.value();
    return true;
}

FlatPtr crypto_mldsa_generate_key_pair(::Crypto::PK::MLDSASize size, ReadonlyBytes seed, KeyHandle** out_public_key, KeyHandle** out_private_key)
{
    auto key_pair = ::Crypto::PK::MLDSA::generate_key_pair(size, seed);
    if (key_pair.is_error())
        return Utf16String::formatted("{}", key_pair.error()).into_raw();
    *out_public_key = new KeyHandle(move(key_pair.value().public_key));
    *out_private_key = new KeyHandle(move(key_pair.value().private_key));
    return 0;
}

void crypto_asn1_key_info_identifier(Asn1Structure structure, void const* key_info, ReadonlySpan<u32>* identifier)
{
    *identifier = key_info_algorithm(structure, key_info).identifier.span();
}

KeyHandle* crypto_mldsa_key_info_read_private_key(void const* key_info)
{
    return new KeyHandle(static_cast<PrivateKeyStorage const*>(key_info)->value.mldsa);
}

KeyHandle* crypto_mldsa_read_public_key(::Crypto::PK::MLDSASize size, ReadonlyBytes bytes)
{
    auto public_key = ::Crypto::PK::MLDSAPublicKey::from_bytes(bytes);
    if (public_key.is_error() || public_key.value().size() != size)
        return nullptr;
    return new KeyHandle(public_key.release_value());
}

void crypto_mldsa_public_bytes(KeyHandle const* key, ReadonlyBytes* bytes)
{
    *bytes = key->visit(
        [](::Crypto::PK::MLDSAPublicKey const& public_key) -> ReadonlyBytes { return public_key.public_key(); },
        [](::Crypto::PK::MLDSAPrivateKey const& private_key) -> ReadonlyBytes { return private_key.public_key(); },
        [](auto const&) -> ReadonlyBytes { VERIFY_NOT_REACHED(); });
}

bool crypto_mldsa_seed_bytes(KeyHandle const* key, void* state, ReceiveEncodedBytes receive)
{
    Array<u8, ::Crypto::PK::MLDSAPrivateKey::seed_size> buffer;
    ScopeGuard const clear_buffer = [&] { secure_zero(buffer.data(), buffer.size()); };
    auto seed = key->get<::Crypto::PK::MLDSAPrivateKey>().seed(buffer);
    if (seed.is_error())
        return false;
    receive(state, seed.value());
    return true;
}

bool crypto_mldsa_export_der(KeyHandle const* key, bool is_public, ReadonlySpan<u32> oid, void* state, ReceiveEncodedBytes receive)
{
    auto result = [&]() -> ErrorOr<ByteBuffer> {
        if (is_public)
            return ::Crypto::PK::wrap_in_subject_public_key_info(key->get<::Crypto::PK::MLDSAPublicKey>().public_key(), oid);
        auto private_key = TRY(key->get<::Crypto::PK::MLDSAPrivateKey>().export_as_der());
        ScopeGuard const clear_private_key = [&] { secure_zero(private_key.data(), private_key.size()); };
        return ::Crypto::PK::wrap_in_private_key_info(private_key, oid);
    }();
    return receive_byte_result(result, state, receive);
}

extern "C" {
FlatPtr crypto_mlkem_generate_key_pair(::Crypto::PK::MLKEMSize, ReadonlyBytes, KeyHandle**, KeyHandle**);
KeyHandle* crypto_mlkem_key_info_read_private_key(void const*);
KeyHandle* crypto_mlkem_read_public_key(ReadonlyBytes);
void crypto_mlkem_public_bytes(KeyHandle const*, void*, ReceiveEncodedBytes);
bool crypto_mlkem_seed_bytes(KeyHandle const*, void*, ReceiveEncodedBytes);
bool crypto_mlkem_export_der(KeyHandle const*, bool, ReadonlySpan<u32>, void*, ReceiveEncodedBytes);
FlatPtr crypto_mlkem_encapsulate(::Crypto::PK::MLKEMSize, KeyHandle const*, void*, void*, ReceiveEncodedBytes);
FlatPtr crypto_mlkem_decapsulate(::Crypto::PK::MLKEMSize, KeyHandle const*, ReadonlyBytes, void*, ReceiveEncodedBytes);
}

static_assert(to_underlying(::Crypto::PK::MLKEMSize::MLKEM512) == 0);
static_assert(to_underlying(::Crypto::PK::MLKEMSize::MLKEM768) == 1);
static_assert(to_underlying(::Crypto::PK::MLKEMSize::MLKEM1024) == 2);

FlatPtr crypto_mlkem_generate_key_pair(::Crypto::PK::MLKEMSize size, ReadonlyBytes seed, KeyHandle** out_public_key, KeyHandle** out_private_key)
{
    auto key_pair = ::Crypto::PK::MLKEM::generate_key_pair(size, seed);
    if (key_pair.is_error())
        return Utf16String::formatted("{}", key_pair.error()).into_raw();
    *out_public_key = new KeyHandle(move(key_pair.value().public_key));
    *out_private_key = new KeyHandle(move(key_pair.value().private_key));
    return 0;
}

KeyHandle* crypto_mlkem_key_info_read_private_key(void const* key_info)
{
    return new KeyHandle(static_cast<PrivateKeyStorage const*>(key_info)->value.mlkem);
}

KeyHandle* crypto_mlkem_read_public_key(ReadonlyBytes bytes)
{
    auto public_key = ::Crypto::PK::MLKEMPublicKey::from_bytes(bytes);
    if (public_key.is_error())
        return nullptr;
    return new KeyHandle(public_key.release_value());
}

void crypto_mlkem_public_bytes(KeyHandle const* key, void* state, ReceiveEncodedBytes receive)
{
    receive(state, key->get<::Crypto::PK::MLKEMPublicKey>().public_key());
}

bool crypto_mlkem_seed_bytes(KeyHandle const* key, void* state, ReceiveEncodedBytes receive)
{
    Array<u8, ::Crypto::PK::MLKEMPrivateKey::seed_size> buffer;
    ScopeGuard const clear_buffer = [&] { secure_zero(buffer.data(), buffer.size()); };
    auto seed = key->get<::Crypto::PK::MLKEMPrivateKey>().seed(buffer);
    if (seed.is_error())
        return false;
    receive(state, seed.value());
    return true;
}

extern "C" bool crypto_ml_private_key_has_seed(KeyHandle const*);

bool crypto_ml_private_key_has_seed(KeyHandle const* key)
{
    return key->visit(
        [](::Crypto::PK::MLDSAPrivateKey const& private_key) { return private_key.has_seed(); },
        [](::Crypto::PK::MLKEMPrivateKey const& private_key) { return private_key.has_seed(); },
        [](auto const&) -> bool { VERIFY_NOT_REACHED(); });
}

bool crypto_mlkem_export_der(KeyHandle const* key, bool is_public, ReadonlySpan<u32> oid, void* state, ReceiveEncodedBytes receive)
{
    auto result = [&]() -> ErrorOr<ByteBuffer> {
        if (is_public)
            return ::Crypto::PK::wrap_in_subject_public_key_info(key->get<::Crypto::PK::MLKEMPublicKey>().public_key(), oid);
        auto private_key = TRY(key->get<::Crypto::PK::MLKEMPrivateKey>().export_as_der());
        ScopeGuard const clear_private_key = [&] { secure_zero(private_key.data(), private_key.size()); };
        return ::Crypto::PK::wrap_in_private_key_info(private_key, oid);
    }();
    return receive_byte_result(result, state, receive);
}

FlatPtr crypto_mlkem_encapsulate(::Crypto::PK::MLKEMSize size, KeyHandle const* key, void* shared_key, void* ciphertext, ReceiveEncodedBytes receive)
{
    auto result = ::Crypto::PK::MLKEM::encapsulate(size, key->get<::Crypto::PK::MLKEMPublicKey>());
    if (result.is_error())
        return Utf16String::formatted("{}", result.error()).into_raw();
    receive(shared_key, result.value().shared_key);
    receive(ciphertext, result.value().ciphertext);
    secure_zero(result.value().shared_key.data(), result.value().shared_key.size());
    return 0;
}

FlatPtr crypto_mlkem_decapsulate(::Crypto::PK::MLKEMSize size, KeyHandle const* key, ReadonlyBytes ciphertext, void* state, ReceiveEncodedBytes receive)
{
    auto result = ::Crypto::PK::MLKEM::decapsulate(size, key->get<::Crypto::PK::MLKEMPrivateKey>(), ciphertext);
    if (result.is_error())
        return Utf16String::formatted("{}", result.error()).into_raw();
    receive(state, result.value());
    secure_zero(result.value().data(), result.value().size());
    return 0;
}

struct Argon2Inputs {
    ReadonlyBytes message;
    ReadonlyBytes nonce;
    u32 parallelism;
    u32 memory;
    u32 passes;
    u32 version;
    OptionalBytes secret_value;
    OptionalBytes associated_data;
    u32 tag_length;
};

extern "C" {
size_t crypto_hash_digest_size(::Crypto::Hash::HashKind);
bool crypto_hkdf_derive_key(::Crypto::Hash::HashKind, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, u32, void*, ReceiveEncodedBytes);
bool crypto_pbkdf2_derive_key(::Crypto::Hash::HashKind, ReadonlyBytes, ReadonlyBytes, u32, u32, void*, ReceiveEncodedBytes);
FlatPtr crypto_argon2_validate_parameters(ReadonlyBytes);
FlatPtr crypto_argon2_derive_key(::Crypto::Hash::Argon2Type, Argon2Inputs const*, void*, ReceiveEncodedBytes);
}

size_t crypto_hash_digest_size(::Crypto::Hash::HashKind hash_kind)
{
    return ::Crypto::Hash::digest_size(hash_kind);
}

bool crypto_hkdf_derive_key(::Crypto::Hash::HashKind kind, ReadonlyBytes salt, ReadonlyBytes key_derivation_key, ReadonlyBytes info, u32 length, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Hash::HKDF hkdf(kind);
    auto result = hkdf.derive_key(salt, key_derivation_key, info, length);
    return receive_byte_result(result, state, receive);
}

bool crypto_pbkdf2_derive_key(::Crypto::Hash::HashKind kind, ReadonlyBytes password, ReadonlyBytes salt, u32 iterations, u32 length, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Hash::PBKDF2 pbkdf2(kind);
    auto result = pbkdf2.derive_key(password, salt, iterations, length);
    return receive_byte_result(result, state, receive);
}

static_assert(to_underlying(::Crypto::Hash::Argon2Type::Argon2d) == 0);
static_assert(to_underlying(::Crypto::Hash::Argon2Type::Argon2i) == 1);
static_assert(to_underlying(::Crypto::Hash::Argon2Type::Argon2id) == 2);

FlatPtr crypto_argon2_validate_parameters(ReadonlyBytes nonce)
{
    auto result = ::Crypto::Hash::Argon2::validate_parameters(nonce);
    if (result.is_error())
        return Utf16String::formatted("{}", result.error()).into_raw();
    return 0;
}

FlatPtr crypto_argon2_derive_key(::Crypto::Hash::Argon2Type kind, Argon2Inputs const* inputs, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Hash::Argon2 algorithm(kind);
    auto result = algorithm.derive_key(inputs->message, inputs->nonce, inputs->parallelism, inputs->memory,
        inputs->passes, inputs->version, inputs->secret_value.to_optional(), inputs->associated_data.to_optional(),
        inputs->tag_length);
    if (result.is_error())
        return Utf16String::formatted("{}", result.error()).into_raw();
    receive(state, result.value());
    return 0;
}

static_assert(to_underlying(::Crypto::Hash::SHAKEKind::CSHAKE128) == 0);
static_assert(to_underlying(::Crypto::Hash::SHAKEKind::CSHAKE256) == 1);

extern "C" FlatPtr crypto_cshake_validate_parameters(u32);
extern "C" FlatPtr crypto_cshake_digest(::Crypto::Hash::SHAKEKind, ReadonlyBytes, u32, OptionalBytes, OptionalBytes, void*, ReceiveEncodedBytes);

FlatPtr crypto_cshake_validate_parameters(u32 output_length)
{
    auto result = ::Crypto::Hash::SHAKE::validate_parameters(output_length);
    if (result.is_error())
        return Utf16String::formatted("{}", result.error()).into_raw();
    return 0;
}

FlatPtr crypto_cshake_digest(::Crypto::Hash::SHAKEKind kind, ReadonlyBytes data, u32 output_length, OptionalBytes customization, OptionalBytes function_name, void* state, ReceiveEncodedBytes receive)
{
    auto maybe_result = ::Crypto::Hash::SHAKE(kind).digest(data, output_length, customization.to_optional(), function_name.to_optional());
    if (maybe_result.is_error())
        return Utf16String::formatted("{}", maybe_result.error()).into_raw();
    receive(state, maybe_result.value());
    return 0;
}

extern "C" FlatPtr crypto_chacha20_poly1305_cipher(bool, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);

FlatPtr crypto_chacha20_poly1305_cipher(bool encrypt, ReadonlyBytes key, ReadonlyBytes iv, ReadonlyBytes input, ReadonlyBytes additional_data, void* state, ReceiveEncodedBytes receive)
{
    auto result = encrypt
        ? ::Crypto::Cipher::ChaCha20Poly1305::encrypt(key, iv, input, additional_data)
        : ::Crypto::Cipher::ChaCha20Poly1305::decrypt(key, iv, input, additional_data);
    if (result.is_error())
        return Utf16String::formatted("{}", result.error()).into_raw();
    receive(state, result.value());
    return 0;
}

extern "C" {
u32 crypto_kmac_default_key_length(::Crypto::Authentication::KMACKind);
bool crypto_kmac_validate_parameters(u32, OptionalBytes);
bool crypto_kmac_sign(::Crypto::Authentication::KMACKind, ReadonlyBytes, ReadonlyBytes, u32, OptionalBytes, void*, ReceiveEncodedBytes);
}

static_assert(to_underlying(::Crypto::Authentication::KMACKind::KMAC128) == 0);
static_assert(to_underlying(::Crypto::Authentication::KMACKind::KMAC256) == 1);

u32 crypto_kmac_default_key_length(::Crypto::Authentication::KMACKind kind)
{
    return ::Crypto::Authentication::KMAC::default_key_length(kind);
}

bool crypto_kmac_validate_parameters(u32 output_length, OptionalBytes customization)
{
    return !::Crypto::Authentication::KMAC::validate_parameters(output_length, customization.to_optional()).is_error();
}

bool crypto_kmac_sign(::Crypto::Authentication::KMACKind kind, ReadonlyBytes key, ReadonlyBytes message, u32 output_length, OptionalBytes customization, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Authentication::KMAC kmac(kind);
    auto result = kmac.sign(key, message, output_length, customization.to_optional());
    return receive_byte_result(result, state, receive);
}

extern "C" KeyHandle* crypto_public_key_for_private_key(KeyHandle const*, FlatPtr name);

KeyHandle* crypto_public_key_for_private_key(KeyHandle const* key, FlatPtr raw_name)
{
    auto result = key->visit(
        [](::Crypto::PK::RSAPrivateKey const& private_key) -> ErrorOr<KeyHandle> {
            return TRY(private_key.public_key());
        },
        [](::Crypto::PK::ECPrivateKey const& private_key) -> ErrorOr<KeyHandle> {
            if (private_key.public_key().has_value())
                return *private_key.public_key();
            return TRY(private_key.derive_public_key());
        },
        [](::Crypto::PK::MLDSAPrivateKey const& private_key) -> ErrorOr<KeyHandle> {
            return TRY(::Crypto::PK::MLDSAPublicKey::from_bytes(private_key.public_key()));
        },
        [](::Crypto::PK::MLKEMPrivateKey const& private_key) -> ErrorOr<KeyHandle> {
            return TRY(::Crypto::PK::MLKEMPublicKey::from_bytes(private_key.public_key()));
        },
        [&](OKPPrivateKey const& private_key) -> ErrorOr<KeyHandle> {
            auto name = Utf16String::from_raw(raw_name);
            auto public_key = TRY([&] -> ErrorOr<ByteBuffer> {
                if (name == "Ed25519"_utf16)
                    return ::Crypto::Curves::Ed25519 {}.generate_public_key(private_key.bytes);
                if (name == "Ed448"_utf16)
                    return ::Crypto::Curves::Ed448 {}.generate_public_key(private_key.bytes);
                if (name == "X25519"_utf16)
                    return ::Crypto::Curves::X25519 {}.generate_public_key(private_key.bytes);
                if (name == "X448"_utf16)
                    return ::Crypto::Curves::X448 {}.generate_public_key(private_key.bytes);
                return Error::from_string_literal("Unsupported algorithm");
            }());
            return OKPPublicKey { move(public_key) };
        },
        [](auto const&) -> ErrorOr<KeyHandle> {
            return Error::from_string_literal("Key is not a private key");
        });
    if (result.is_error())
        return nullptr;
    return new KeyHandle(result.release_value());
}

extern "C" {
void crypto_fill_with_random(Bytes);
void crypto_generate_random_uuid(void*, ReceiveEncodedBytes);
}

void crypto_fill_with_random(Bytes bytes)
{
    fill_with_random(bytes);
}

void crypto_generate_random_uuid(void* state, ReceiveEncodedBytes receive)
{
    auto uuid = generate_random_uuid();
    receive(state, uuid.bytes());
}

}
