/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Memory.h>
#include <AK/Random.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/ASN1/Constants.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Authentication/HMAC.h>
#include <LibCrypto/Authentication/KMAC.h>
#include <LibCrypto/Certificate/Certificate.h>
#include <LibCrypto/Cipher/AES.h>
#include <LibCrypto/Cipher/ChaCha.h>
#include <LibCrypto/Curves/EdwardsCurve.h>
#include <LibCrypto/Curves/SECPxxxr1.h>
#include <LibCrypto/Hash/Argon2.h>
#include <LibCrypto/Hash/HKDF.h>
#include <LibCrypto/Hash/HashManager.h>
#include <LibCrypto/Hash/PBKDF2.h>
#include <LibCrypto/Hash/SHA1.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibCrypto/Hash/SHAKE.h>
#include <LibCrypto/KeyHandle.h>
#include <LibCrypto/PK/MLDSA.h>
#include <LibCrypto/PK/MLKEM.h>
#include <LibCrypto/PK/RSA.h>

namespace Crypto {

template<typename T>
struct Asn1Storage final {
    AK_ALLOC_WITH_KMALLOC;

    T value;
};

using SubjectPublicKeyStorage = Asn1Storage<Certificate::SubjectPublicKey>;
using PrivateKeyStorage = Asn1Storage<Certificate::PrivateKey>;
using OctetStringStorage = Asn1Storage<StringView>;

extern "C" {
using ReceiveEncodedBytes = void (*)(void*, ReadonlyBytes);
int crypto_encode_base64url(ReadonlyBytes, ReceiveEncodedBytes, void*);

enum class Base64DecodeResult : u8 {
    Success,
    OutOfMemory,
    InvalidBase64,
    NonAscii,
};
Base64DecodeResult crypto_decode_base64url(FlatPtr, void*, ReceiveEncodedBytes, FlatPtr*);

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

int crypto_encode_base64url(ReadonlyBytes bytes, ReceiveEncodedBytes receive, void* state)
{
    auto encoded = encode_base64url(bytes, AK::OmitPadding::Yes);
    if (encoded.is_error())
        return encoded.error().code();
    receive(state, encoded.value().bytes_as_string_view().bytes());
    return 0;
}

Base64DecodeResult crypto_decode_base64url(FlatPtr raw, void* state, ReceiveEncodedBytes receive, FlatPtr* error_message)
{
    auto base64_url_string = Utf16String::from_raw(raw);
    auto base64_bytes_or_error = decode_base64url(base64_url_string.to_byte_string(), AK::LastChunkHandling::Loose);
    if (base64_bytes_or_error.is_error()) {
        if (base64_bytes_or_error.error().code() == ENOMEM)
            return Base64DecodeResult::OutOfMemory;
        *error_message = Utf16String::formatted("{}", base64_bytes_or_error.release_error()).into_raw();
        return Base64DecodeResult::InvalidBase64;
    }
    receive(state, base64_bytes_or_error.value());
    return Base64DecodeResult::Success;
}

FlatPtr crypto_decode_asn1_structure(ReadonlyBytes data, Asn1Structure structure, void* out, bool* all_consumed)
{
    ::Crypto::ASN1::Decoder decoder(data);
    switch (structure) {
    case Asn1Structure::SubjectPublicKeyInfo: {
        auto subject_public_key = ::Crypto::Certificate::parse_subject_public_key_info(decoder);
        if (subject_public_key.is_error())
            return Utf16String::formatted("{}", subject_public_key.release_error()).into_raw();
        static_cast<SubjectPublicKeyStorage*>(out)->value = subject_public_key.release_value();
        break;
    }
    case Asn1Structure::PrivateKeyInfo: {
        auto private_key = ::Crypto::Certificate::parse_private_key_info(decoder);
        if (private_key.is_error())
            return Utf16String::formatted("{}", private_key.release_error()).into_raw();
        static_cast<PrivateKeyStorage*>(out)->value = private_key.release_value();
        break;
    }
    case Asn1Structure::OctetString: {
        auto octet_string = decoder.read<StringView>(::Crypto::ASN1::Class::Universal, ::Crypto::ASN1::Kind::OctetString);
        if (octet_string.is_error())
            return Utf16String::formatted("{}", octet_string.error()).into_raw();
        static_cast<OctetStringStorage*>(out)->value = octet_string.release_value();
        break;
    }
    }
    *all_consumed = decoder.eof();
    return 0;
}

extern "C" {
void* crypto_asn1_allocate_key_info(Asn1Structure);
void crypto_asn1_delete_key_info(Asn1Structure, void*);
bool crypto_rsa_key_info_has_encryption_oid(Asn1Structure, void const*);
KeyHandle* crypto_rsa_key_info_read_key(Asn1Structure, void const*);
bool crypto_rsa_key_is_valid(KeyHandle const*, bool*);
bool crypto_rsa_validate_public_exponent(ReadonlyBytes, u32);
bool crypto_rsa_validate_modulus_length(u32);
bool crypto_rsa_generate_key_pair(u32, ReadonlyBytes, KeyHandle**, KeyHandle**);
bool crypto_rsa_export_public_exponent(ReadonlyBytes, void*, ReceiveEncodedBytes);
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
    storage->value.rsa.clear_sensitive_data();
    storage->value.ec.clear_sensitive_data();
    storage->value.mldsa.clear_sensitive_data();
    storage->value.mlkem.clear_sensitive_data();
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

bool crypto_rsa_validate_public_exponent(ReadonlyBytes bytes, u32 modulus_length)
{
    auto public_exponent = ::Crypto::UnsignedBigInteger::import_data(bytes);
    return public_exponent >= ::Crypto::UnsignedBigInteger { 3 }
    && public_exponent.is_odd()
        && public_exponent.plus(1).one_based_index_of_highest_set_bit() <= modulus_length;
}

bool crypto_rsa_validate_modulus_length(u32 modulus_length)
{
    return !::Crypto::PK::RSA::validate_key_generation_parameters(modulus_length).is_error();
}

bool crypto_rsa_generate_key_pair(u32 modulus_length, ReadonlyBytes public_exponent, KeyHandle** out_public_key, KeyHandle** out_private_key)
{
    auto key_pair = ::Crypto::PK::RSA::generate_key_pair(modulus_length, ::Crypto::UnsignedBigInteger::import_data(public_exponent));
    if (key_pair.is_error())
        return false;
    *out_public_key = new KeyHandle(move(key_pair.value().public_key));
    *out_private_key = new KeyHandle(move(key_pair.value().private_key));
    return true;
}

static ErrorOr<ByteBuffer> export_rsa_exponent(::Crypto::UnsignedBigInteger const& exponent)
{
    auto bytes = TRY(ByteBuffer::create_uninitialized(exponent.byte_length()));
    return ByteBuffer::copy(exponent.export_data(bytes.span()));
}

bool crypto_rsa_export_public_exponent(ReadonlyBytes public_exponent, void* state, ReceiveEncodedBytes receive)
{
    auto bytes = export_rsa_exponent(::Crypto::UnsignedBigInteger::import_data(public_exponent));
    return receive_byte_result(bytes, state, receive);
}

bool crypto_rsa_read_key_metadata(KeyHandle const* key, u32* modulus_length, void* state, ReceiveEncodedBytes receive)
{
    auto result = key->visit(
        [&](::Crypto::PK::RSAPublicKey const& public_key) {
            *modulus_length = public_key.modulus().byte_length() * 8;
            return export_rsa_exponent(public_key.public_exponent());
        },
        [&](::Crypto::PK::RSAPrivateKey const& private_key) {
            *modulus_length = private_key.modulus().byte_length() * 8;
            return export_rsa_exponent(private_key.public_exponent());
        },
        [](auto const&) -> ErrorOr<ByteBuffer> { VERIFY_NOT_REACHED(); });
    return receive_byte_result(result, state, receive);
}

KeyHandle* crypto_rsa_key_from_jwk_components(bool is_private, ReadonlySpan<ReadonlyBytes> components)
{
    auto n = ::Crypto::UnsignedBigInteger::import_data(components[0]);
    auto e = ::Crypto::UnsignedBigInteger::import_data(components[1]);
    if (!is_private)
        return new KeyHandle(::Crypto::PK::RSAPublicKey(move(n), move(e)));

    auto d = ::Crypto::UnsignedBigInteger::import_data(components[2]);
    if (components.size() == 3)
        return new KeyHandle(::Crypto::PK::RSAPrivateKey(move(n), move(d), move(e)));

    auto p = ::Crypto::UnsignedBigInteger::import_data(components[3]);
    auto q = ::Crypto::UnsignedBigInteger::import_data(components[4]);
    auto dp = ::Crypto::UnsignedBigInteger::import_data(components[5]);
    auto dq = ::Crypto::UnsignedBigInteger::import_data(components[6]);
    auto qi = ::Crypto::UnsignedBigInteger::import_data(components[7]);
    return new KeyHandle(::Crypto::PK::RSAPrivateKey(move(n), move(d), move(e), move(p), move(q), move(dp), move(dq), move(qi)));
}

bool crypto_rsa_export_der(KeyHandle const* key, bool is_public, void* state, ReceiveEncodedBytes receive)
{
    auto result = is_public
        ? ::Crypto::PK::wrap_in_subject_public_key_info(key->get<::Crypto::PK::RSAPublicKey>(), ::Crypto::ASN1::rsa_encryption_oid, nullptr)
        : ::Crypto::PK::wrap_in_private_key_info(key->get<::Crypto::PK::RSAPrivateKey>(), ::Crypto::ASN1::rsa_encryption_oid, nullptr);
    return receive_byte_result(result, state, receive);
}

bool crypto_rsa_export_jwk_integer(KeyHandle const* key, u8 component, void* state, ReceiveEncodedBytes receive)
{
    auto const& integer = key->visit(
        [&](::Crypto::PK::RSAPublicKey const& public_key) -> ::Crypto::UnsignedBigInteger const& {
            VERIFY(component < 2);
            return component == 0 ? public_key.modulus() : public_key.public_exponent();
        },
        [&](::Crypto::PK::RSAPrivateKey const& private_key) -> ::Crypto::UnsignedBigInteger const& {
            switch (component) {
            case 0:
                return private_key.modulus();
            case 1:
                return private_key.public_exponent();
            case 2:
                return private_key.private_exponent();
            case 3:
                return private_key.prime1();
            case 4:
                return private_key.prime2();
            case 5:
                return private_key.exponent1();
            case 6:
                return private_key.exponent2();
            case 7:
                return private_key.coefficient();
            }
            VERIFY_NOT_REACHED();
        },
        [](auto const&) -> ::Crypto::UnsignedBigInteger const& { VERIFY_NOT_REACHED(); });
    auto bytes = ByteBuffer::create_uninitialized(integer.byte_length());
    if (bytes.is_error())
        return false;
    receive(state, integer.export_data(bytes.value().span()));
    return true;
}

bool crypto_rsa_oaep_cipher(bool encrypt, ::Crypto::Hash::HashKind hash_kind, KeyHandle const* key, ReadonlyBytes label, ReadonlyBytes input, void* state, ReceiveEncodedBytes receive)
{
    auto result = [&]() {
        if (encrypt) {
            ::Crypto::PK::RSA_OAEP_EME rsa { hash_kind, key->get<::Crypto::PK::RSAPublicKey>() };
            rsa.set_label(label);
            return rsa.encrypt(input);
        }
        ::Crypto::PK::RSA_OAEP_EME rsa { hash_kind, key->get<::Crypto::PK::RSAPrivateKey>() };
        rsa.set_label(label);
        return rsa.decrypt(input);
    }();
    return receive_byte_result(result, state, receive);
}

bool crypto_rsa_pss_sign(::Crypto::Hash::HashKind hash_kind, KeyHandle const* key, u32 salt_length, ReadonlyBytes message, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::PK::RSA_PSS_EMSA rsa { hash_kind, key->get<::Crypto::PK::RSAPrivateKey>() };
    rsa.set_salt_length(salt_length);
    auto result = rsa.sign(message);
    return receive_byte_result(result, state, receive);
}

bool crypto_rsa_pss_verify(::Crypto::Hash::HashKind hash_kind, KeyHandle const* key, u32 salt_length, ReadonlyBytes message, ReadonlyBytes signature, bool* verified)
{
    ::Crypto::PK::RSA_PSS_EMSA rsa { hash_kind, key->get<::Crypto::PK::RSAPublicKey>() };
    rsa.set_salt_length(salt_length);
    auto result = rsa.verify(message, signature);
    if (result.is_error())
        return false;
    *verified = result.value();
    return true;
}

bool crypto_rsa_pkcs1_sign(::Crypto::Hash::HashKind hash_kind, KeyHandle const* key, ReadonlyBytes message, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::PK::RSA_PKCS1_EMSA rsa { hash_kind, key->get<::Crypto::PK::RSAPrivateKey>() };
    auto result = rsa.sign(message);
    return receive_byte_result(result, state, receive);
}

bool crypto_rsa_pkcs1_verify(::Crypto::Hash::HashKind hash_kind, KeyHandle const* key, ReadonlyBytes message, ReadonlyBytes signature, bool* verified)
{
    ::Crypto::PK::RSA_PKCS1_EMSA rsa { hash_kind, key->get<::Crypto::PK::RSAPublicKey>() };
    auto result = rsa.verify(message, signature);
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

static_assert(to_underlying(::Crypto::Hash::HashKind::SHA1) == 4);
static_assert(to_underlying(::Crypto::Hash::HashKind::SHA256) == 5);
static_assert(to_underlying(::Crypto::Hash::HashKind::SHA384) == 6);
static_assert(to_underlying(::Crypto::Hash::HashKind::SHA512) == 7);
static_assert(to_underlying(::Crypto::Hash::HashKind::SHA3_256) == 8);
static_assert(to_underlying(::Crypto::Hash::HashKind::SHA3_384) == 9);
static_assert(to_underlying(::Crypto::Hash::HashKind::SHA3_512) == 10);

extern "C" bool crypto_sha_digest(::Crypto::Hash::HashKind, ReadonlyBytes, void*, ReceiveEncodedBytes);

bool crypto_sha_digest(::Crypto::Hash::HashKind hash_kind, ReadonlyBytes data, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Hash::Manager hash { hash_kind };
    hash.update(data);

    auto digest = hash.digest();
    auto result_buffer = ByteBuffer::copy(digest.immutable_data(), hash.digest_size());
    return receive_byte_result(result_buffer, state, receive);
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

static ::Crypto::Curves::SECPxxxr1 ec_curve(EcCurve curve)
{
    switch (curve) {
    case EcCurve::P256:
        return ::Crypto::Curves::SECP256r1 {};
    case EcCurve::P384:
        return ::Crypto::Curves::SECP384r1 {};
    case EcCurve::P521:
        return ::Crypto::Curves::SECP521r1 {};
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
u8 crypto_ec_sign(EcCurve, KeyHandle const*, ReadonlyBytes, FlatPtr*, void*, void*, ReceiveEncodedBytes);
FlatPtr crypto_ec_verify(EcCurve, KeyHandle const*, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, bool*);
size_t crypto_ec_public_key_scalar_size(KeyHandle const*);
u8 crypto_ec_compute_coordinate(EcCurve, KeyHandle const*, KeyHandle const*, FlatPtr*, void*, ReceiveEncodedBytes);
bool crypto_ec_key_info_has_public_key_oid(Asn1Structure, void const*);
bool crypto_ec_key_info_parameters(Asn1Structure, void const*, ReadonlySpan<u32>*);
bool crypto_ec_private_key_info_parameters_match(void const*);
KeyHandle* crypto_ec_key_info_read_key(Asn1Structure, void const*);
KeyHandle* crypto_ec_key_from_coordinates(ReadonlyBytes, ReadonlyBytes, size_t, OptionalBytes);
KeyHandle* crypto_ec_parse_raw_public_key(ReadonlyBytes);
bool crypto_ec_key_is_valid(EcCurve, KeyHandle const*, bool*);
bool crypto_ec_export_der(EcCurve, KeyHandle const*, bool, void*, ReceiveEncodedBytes);
bool crypto_ec_export_uncompressed(KeyHandle const*, void*, ReceiveEncodedBytes);
bool crypto_ec_export_public_coordinates(EcCurve, KeyHandle const*, void*, void*, ReceiveEncodedBytes);
bool crypto_ec_export_private_scalar(KeyHandle const*, void*, ReceiveEncodedBytes);
}

bool crypto_ec_generate_key_pair(EcCurve curve_id, KeyHandle** out_public_key, KeyHandle** out_private_key)
{
    auto curve = ec_curve(curve_id);
    auto maybe_private_key_data = curve.generate_private_key();
    if (maybe_private_key_data.is_error())
        return false;
    auto private_key_data = maybe_private_key_data.release_value();
    auto maybe_public_key_data = curve.generate_public_key(private_key_data);
    if (maybe_public_key_data.is_error())
        return false;
    auto public_key_data = maybe_public_key_data.release_value();
    auto public_key = ::Crypto::PK::ECPublicKey { public_key_data };
    auto private_key = ::Crypto::PK::ECPrivateKey { move(private_key_data), public_key_data.size, {}, public_key };
    *out_public_key = new KeyHandle(move(public_key));
    *out_private_key = new KeyHandle(move(private_key));
    return true;
}

u8 crypto_ec_sign(EcCurve curve_id, KeyHandle const* key, ReadonlyBytes digest, FlatPtr* error, void* r, void* s, ReceiveEncodedBytes receive)
{
    auto signature = ec_curve(curve_id).sign(digest, key->get<::Crypto::PK::ECPrivateKey>().d());
    if (signature.is_error()) {
        *error = Utf16String::from_utf8(signature.error().string_literal()).into_raw();
        return 1;
    }
    auto r_bytes = signature.value().r_bytes();
    if (r_bytes.is_error())
        return 2;
    receive(r, r_bytes.value());
    auto s_bytes = signature.value().s_bytes();
    if (s_bytes.is_error())
        return 2;
    receive(s, s_bytes.value());
    return 0;
}

FlatPtr crypto_ec_verify(EcCurve curve_id, KeyHandle const* key, ReadonlyBytes digest, ReadonlyBytes r_bytes, ReadonlyBytes s_bytes, bool* verified)
{
    auto const& public_key = key->get<::Crypto::PK::ECPublicKey>();
    auto r = ::Crypto::UnsignedBigInteger::import_data(r_bytes);
    auto s = ::Crypto::UnsignedBigInteger::import_data(s_bytes);
    auto result = ec_curve(curve_id).verify(digest, public_key.to_secpxxxr1_point(), { move(r), move(s), public_key.scalar_size() });
    if (result.is_error())
        return Utf16String::from_utf8(result.error().string_literal()).into_raw();
    *verified = result.value();
    return 0;
}

size_t crypto_ec_public_key_scalar_size(KeyHandle const* key)
{
    return key->get<::Crypto::PK::ECPublicKey>().scalar_size();
}

u8 crypto_ec_compute_coordinate(EcCurve curve_id, KeyHandle const* key, KeyHandle const* public_key, FlatPtr* error, void* state, ReceiveEncodedBytes receive)
{
    auto const& private_key_data = key->get<::Crypto::PK::ECPrivateKey>();
    auto const& public_key_data = public_key->get<::Crypto::PK::ECPublicKey>();
    auto secret = ec_curve(curve_id).compute_coordinate(private_key_data.d(), public_key_data.to_secpxxxr1_point());
    if (secret.is_error()) {
        *error = Utf16String::formatted("{}", secret.error()).into_raw();
        return 1;
    }
    auto secret_data = secret.value().to_uncompressed();
    if (secret_data.is_error())
        return 2;
    receive(state, secret_data.value());
    return 0;
}

static ::Crypto::Certificate::AlgorithmIdentifier const& key_info_algorithm(Asn1Structure structure, void const* key_info)
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

KeyHandle* crypto_ec_key_from_coordinates(ReadonlyBytes x, ReadonlyBytes y, size_t coord_size, OptionalBytes d)
{
    auto public_key = ::Crypto::PK::ECPublicKey { ::Crypto::UnsignedBigInteger::import_data(x), ::Crypto::UnsignedBigInteger::import_data(y), coord_size };
    if (!d.has_value)
        return new KeyHandle(move(public_key));
    return new KeyHandle(::Crypto::PK::ECPrivateKey { ::Crypto::UnsignedBigInteger::import_data(d.bytes), coord_size, {}, move(public_key) });
}

KeyHandle* crypto_ec_parse_raw_public_key(ReadonlyBytes data)
{
    auto result = ::Crypto::PK::EC::parse_ec_key(data, false, {});
    if (result.is_error())
        return nullptr;
    return new KeyHandle(move(result.value().public_key));
}

bool crypto_ec_key_is_valid(EcCurve curve_id, KeyHandle const* key, bool* valid)
{
    auto curve = ec_curve(curve_id);
    auto result = key->visit(
        [&](::Crypto::PK::ECPublicKey const& public_key) {
            return curve.is_valid_point(public_key.to_secpxxxr1_point());
        },
        [&](::Crypto::PK::ECPrivateKey const& private_key) -> ErrorOr<bool> {
            if (!private_key.public_key().has_value())
                return true;
            return curve.is_valid_point(private_key.public_key()->to_secpxxxr1_point(), private_key.d());
        },
        [](auto const&) -> ErrorOr<bool> { VERIFY_NOT_REACHED(); });
    if (result.is_error())
        return false;
    *valid = result.value();
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
        return ::Crypto::PK::wrap_in_private_key_info(key->get<::Crypto::PK::ECPrivateKey>(), ::Crypto::ASN1::ec_public_key_encryption_oid, ec_params);
    }();
    return receive_byte_result(result, state, receive);
}

bool crypto_ec_export_uncompressed(KeyHandle const* key, void* state, ReceiveEncodedBytes receive)
{
    auto result = key->get<::Crypto::PK::ECPublicKey>().to_uncompressed();
    return receive_byte_result(result, state, receive);
}

bool crypto_ec_export_public_coordinates(EcCurve curve_id, KeyHandle const* key, void* x, void* y, ReceiveEncodedBytes receive)
{
    auto result = key->visit(
        [&](::Crypto::PK::ECPublicKey const& public_key) -> ErrorOr<void> {
            auto x_bytes = TRY(public_key.x_bytes());
            receive(x, x_bytes);
            auto y_bytes = TRY(public_key.y_bytes());
            receive(y, y_bytes);
            return {};
        },
        [&](::Crypto::PK::ECPrivateKey const& private_key) -> ErrorOr<void> {
            auto public_key = TRY(ec_curve(curve_id).generate_public_key(private_key.d()));
            auto x_bytes = TRY(public_key.x_bytes());
            auto y_bytes = TRY(public_key.y_bytes());
            receive(x, x_bytes);
            receive(y, y_bytes);
            return {};
        },
        [](auto const&) -> ErrorOr<void> { VERIFY_NOT_REACHED(); });
    return !result.is_error();
}

bool crypto_ec_export_private_scalar(KeyHandle const* key, void* state, ReceiveEncodedBytes receive)
{
    auto result = key->get<::Crypto::PK::ECPrivateKey>().d_bytes();
    return receive_byte_result(result, state, receive);
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
    *bytes = string->value.bytes();
}

bool crypto_okp_export_der(OkpCurve curve, bool is_public, ReadonlyBytes key, void* state, ReceiveEncodedBytes receive)
{
    auto result = [&]() -> ErrorOr<ByteBuffer> {
        auto oid = okp_curve_oid(curve);
        if (is_public)
            return ::Crypto::PK::wrap_in_subject_public_key_info(TRY(ByteBuffer::copy(key)), oid);
        ::Crypto::ASN1::Encoder encoder;
        TRY(encoder.write(key));
        return ::Crypto::PK::wrap_in_private_key_info(encoder.finish(), oid);
    }();
    return receive_byte_result(result, state, receive);
}

extern "C" {
void crypto_hmac_sign(::Crypto::Hash::HashKind, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool crypto_timing_safe_compare(ReadonlyBytes, ReadonlyBytes);
}

static_assert(::Crypto::Hash::SHA1::block_size() == 64);
static_assert(::Crypto::Hash::SHA256::block_size() == 64);
static_assert(::Crypto::Hash::SHA384::block_size() == 128);
static_assert(::Crypto::Hash::SHA512::block_size() == 128);

void crypto_hmac_sign(::Crypto::Hash::HashKind hash_kind, ReadonlyBytes key, ReadonlyBytes message, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Authentication::HMAC hmac(hash_kind, key);
    auto mac = hmac.process(message);
    receive(state, mac);
}

bool crypto_timing_safe_compare(ReadonlyBytes left, ReadonlyBytes right)
{
    VERIFY(left.size() == right.size());
    return timing_safe_compare(left.data(), right.data(), left.size());
}

extern "C" {
bool crypto_mldsa_sign(::Crypto::PK::MLDSASize, KeyHandle const*, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool crypto_mldsa_verify(::Crypto::PK::MLDSASize, KeyHandle const*, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, bool*);
FlatPtr crypto_mldsa_generate_key_pair(::Crypto::PK::MLDSASize, ReadonlyBytes, KeyHandle**, KeyHandle**);
void crypto_asn1_key_info_identifier(Asn1Structure, void const*, ReadonlySpan<u32>*);
KeyHandle* crypto_mldsa_key_info_read_private_key(void const*);
KeyHandle* crypto_mldsa_read_public_key(ReadonlyBytes);
void crypto_mldsa_public_bytes(KeyHandle const*, ReadonlyBytes*);
void crypto_mldsa_seed_bytes(KeyHandle const*, ReadonlyBytes*);
bool crypto_mldsa_export_der(KeyHandle const*, bool, ReadonlySpan<u32>, void*, ReceiveEncodedBytes);
}

static_assert(to_underlying(::Crypto::PK::MLDSA44) == 0);
static_assert(to_underlying(::Crypto::PK::MLDSA65) == 1);
static_assert(to_underlying(::Crypto::PK::MLDSA87) == 2);

bool crypto_mldsa_sign(::Crypto::PK::MLDSASize size, KeyHandle const* key, ReadonlyBytes message, ReadonlyBytes context, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::PK::MLDSA lattice(size, key->get<::Crypto::PK::MLDSAPrivateKey>(), MUST(ByteBuffer::copy(context)));
    auto result = lattice.sign(message);
    return receive_byte_result(result, state, receive);
}

bool crypto_mldsa_verify(::Crypto::PK::MLDSASize size, KeyHandle const* key, ReadonlyBytes message, ReadonlyBytes signature, ReadonlyBytes context, bool* verified)
{
    ::Crypto::PK::MLDSA lattice(size, key->get<::Crypto::PK::MLDSAPublicKey>(), MUST(ByteBuffer::copy(context)));
    auto result = lattice.verify(message, signature);
    if (result.is_error())
        return false;
    *verified = result.value();
    return true;
}

FlatPtr crypto_mldsa_generate_key_pair(::Crypto::PK::MLDSASize size, ReadonlyBytes seed, KeyHandle** out_public_key, KeyHandle** out_private_key)
{
    auto key_pair = ::Crypto::PK::MLDSA::generate_key_pair(size, MUST(ByteBuffer::copy(seed)));
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

KeyHandle* crypto_mldsa_read_public_key(ReadonlyBytes bytes)
{
    return new KeyHandle(::Crypto::PK::MLDSAPublicKey { MUST(ByteBuffer::copy(bytes)) });
}

void crypto_mldsa_public_bytes(KeyHandle const* key, ReadonlyBytes* bytes)
{
    *bytes = key->visit(
        [](::Crypto::PK::MLDSAPublicKey const& public_key) -> ReadonlyBytes { return public_key.public_key(); },
        [](::Crypto::PK::MLDSAPrivateKey const& private_key) -> ReadonlyBytes { return private_key.public_key(); },
        [](auto const&) -> ReadonlyBytes { VERIFY_NOT_REACHED(); });
}

void crypto_mldsa_seed_bytes(KeyHandle const* key, ReadonlyBytes* bytes)
{
    *bytes = key->get<::Crypto::PK::MLDSAPrivateKey>().seed();
}

bool crypto_mldsa_export_der(KeyHandle const* key, bool is_public, ReadonlySpan<u32> oid, void* state, ReceiveEncodedBytes receive)
{
    auto result = is_public
        ? ::Crypto::PK::wrap_in_subject_public_key_info(key->get<::Crypto::PK::MLDSAPublicKey>().public_key(), oid)
        : ::Crypto::PK::wrap_in_private_key_info(key->get<::Crypto::PK::MLDSAPrivateKey>(), oid);
    return receive_byte_result(result, state, receive);
}

extern "C" {
FlatPtr crypto_mlkem_generate_key_pair(::Crypto::PK::MLKEMSize, ReadonlyBytes, KeyHandle**, KeyHandle**);
KeyHandle* crypto_mlkem_key_info_read_private_key(void const*);
KeyHandle* crypto_mlkem_read_public_key(ReadonlyBytes);
void crypto_mlkem_public_bytes(KeyHandle const*, void*, ReceiveEncodedBytes);
void crypto_mlkem_seed_bytes(KeyHandle const*, ReadonlyBytes*);
bool crypto_mlkem_export_der(KeyHandle const*, bool, ReadonlySpan<u32>, void*, ReceiveEncodedBytes);
FlatPtr crypto_mlkem_encapsulate(::Crypto::PK::MLKEMSize, KeyHandle const*, void*, void*, ReceiveEncodedBytes);
FlatPtr crypto_mlkem_decapsulate(::Crypto::PK::MLKEMSize, KeyHandle const*, ReadonlyBytes, void*, ReceiveEncodedBytes);
}

static_assert(to_underlying(::Crypto::PK::MLKEMSize::MLKEM512) == 0);
static_assert(to_underlying(::Crypto::PK::MLKEMSize::MLKEM768) == 1);
static_assert(to_underlying(::Crypto::PK::MLKEMSize::MLKEM1024) == 2);

FlatPtr crypto_mlkem_generate_key_pair(::Crypto::PK::MLKEMSize size, ReadonlyBytes seed, KeyHandle** out_public_key, KeyHandle** out_private_key)
{
    auto key_pair = ::Crypto::PK::MLKEM::generate_key_pair(size, MUST(ByteBuffer::copy(seed)));
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
    return new KeyHandle(::Crypto::PK::MLKEMPublicKey { MUST(ByteBuffer::copy(bytes)) });
}

void crypto_mlkem_public_bytes(KeyHandle const* key, void* state, ReceiveEncodedBytes receive)
{
    auto public_key = key->get<::Crypto::PK::MLKEMPublicKey>().public_key();
    receive(state, public_key);
}

void crypto_mlkem_seed_bytes(KeyHandle const* key, ReadonlyBytes* bytes)
{
    *bytes = key->get<::Crypto::PK::MLKEMPrivateKey>().seed();
}

bool crypto_mlkem_export_der(KeyHandle const* key, bool is_public, ReadonlySpan<u32> oid, void* state, ReceiveEncodedBytes receive)
{
    auto result = is_public
        ? ::Crypto::PK::wrap_in_subject_public_key_info(key->get<::Crypto::PK::MLKEMPublicKey>().public_key(), oid)
        : ::Crypto::PK::wrap_in_private_key_info(key->get<::Crypto::PK::MLKEMPrivateKey>(), oid);
    return receive_byte_result(result, state, receive);
}

FlatPtr crypto_mlkem_encapsulate(::Crypto::PK::MLKEMSize size, KeyHandle const* key, void* shared_key, void* ciphertext, ReceiveEncodedBytes receive)
{
    auto result = ::Crypto::PK::MLKEM::encapsulate(size, key->get<::Crypto::PK::MLKEMPublicKey>());
    if (result.is_error())
        return Utf16String::formatted("{}", result.error()).into_raw();
    receive(shared_key, result.value().shared_key);
    receive(ciphertext, result.value().ciphertext);
    return 0;
}

FlatPtr crypto_mlkem_decapsulate(::Crypto::PK::MLKEMSize size, KeyHandle const* key, ReadonlyBytes ciphertext, void* state, ReceiveEncodedBytes receive)
{
    auto result = ::Crypto::PK::MLKEM::decapsulate(size, key->get<::Crypto::PK::MLKEMPrivateKey>(), MUST(ByteBuffer::copy(ciphertext)));
    if (result.is_error())
        return Utf16String::formatted("{}", result.error()).into_raw();
    receive(state, result.value());
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
    return ::Crypto::Hash::Manager { hash_kind }.digest_size();
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

extern "C" FlatPtr crypto_cshake_validate_parameters(u32, OptionalBytes, OptionalBytes);
extern "C" FlatPtr crypto_cshake_digest(::Crypto::Hash::SHAKEKind, ReadonlyBytes, u32, OptionalBytes, OptionalBytes, void*, ReceiveEncodedBytes);

FlatPtr crypto_cshake_validate_parameters(u32 output_length, OptionalBytes customization, OptionalBytes function_name)
{
    auto result = ::Crypto::Hash::SHAKE::validate_parameters(output_length, customization.to_optional(), function_name.to_optional());
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

extern "C" KeyHandle* crypto_public_key_for_private_key(KeyHandle const*, FlatPtr name, FlatPtr named_curve);

KeyHandle* crypto_public_key_for_private_key(KeyHandle const* key, FlatPtr raw_name, FlatPtr raw_named_curve)
{
    auto result = key->visit(
        [](::Crypto::PK::RSAPrivateKey const& private_key) -> ErrorOr<KeyHandle> {
            return ::Crypto::PK::RSAPublicKey { private_key.modulus(), private_key.public_exponent() };
        },
        [&](::Crypto::PK::ECPrivateKey const& private_key) -> ErrorOr<KeyHandle> {
            if (private_key.public_key().has_value())
                return *private_key.public_key();

            VERIFY(raw_named_curve);
            auto named_curve = Utf16String::from_raw(raw_named_curve);
            auto point = TRY([&] -> ErrorOr<::Crypto::Curves::SECPxxxr1Point> {
                if (named_curve == "P-256"_utf16)
                    return ::Crypto::Curves::SECP256r1 {}.generate_public_key(private_key.d());
                if (named_curve == "P-384"_utf16)
                    return ::Crypto::Curves::SECP384r1 {}.generate_public_key(private_key.d());
                if (named_curve == "P-521"_utf16)
                    return ::Crypto::Curves::SECP521r1 {}.generate_public_key(private_key.d());
                return Error::from_string_literal("Unsupported named curve");
            }());
            return ::Crypto::PK::ECPublicKey { move(point) };
        },
        [](::Crypto::PK::MLDSAPrivateKey const& private_key) -> ErrorOr<KeyHandle> {
            return ::Crypto::PK::MLDSAPublicKey { private_key.public_key() };
        },
        [](::Crypto::PK::MLKEMPrivateKey const& private_key) -> ErrorOr<KeyHandle> {
            return ::Crypto::PK::MLKEMPublicKey { private_key.public_key() };
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
