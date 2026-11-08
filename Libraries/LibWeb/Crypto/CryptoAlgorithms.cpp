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
#include <AK/HashTable.h>
#include <AK/Memory.h>
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
#include <LibCrypto/PK/MLDSA.h>
#include <LibCrypto/PK/MLKEM.h>
#include <LibCrypto/PK/RSA.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Crypto/CryptoAlgorithms.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace Web::Crypto {

extern "C" {
using ReceiveKeyUsages = void (*)(void*, ReadonlySpan<KeyUsage>);
void web_rust_crypto_usage_intersection(ReadonlySpan<KeyUsage>, ReadonlySpan<KeyUsage>, void*, ReceiveKeyUsages);
void web_rust_crypto_normalized_key_usages(ReadonlySpan<KeyUsage>, void*, ReceiveKeyUsages);

using ReceiveEncodedBytes = void (*)(void*, ReadonlyBytes);
int web_crypto_encode_base64url(ReadonlyBytes, ReceiveEncodedBytes, void*);
int web_rust_crypto_base64_url_bytes_encode(ReadonlyBytes, FlatPtr*);

enum class Base64DecodeResult : u8 {
    Success,
    OutOfMemory,
    InvalidBase64,
    NonAscii,
};
Base64DecodeResult web_crypto_decode_base64url(FlatPtr, ByteBuffer*, void*, ReceiveEncodedBytes, FlatPtr*);
Base64DecodeResult web_rust_crypto_base64_url_bytes_decode(FlatPtr, ByteBuffer*, FlatPtr*);

enum class Asn1Structure : u8 {
    SubjectPublicKeyInfo,
    PrivateKeyInfo,
    OctetString,
};
FlatPtr web_crypto_decode_asn1_structure(ReadonlyBytes, Asn1Structure, void*, bool*);
FlatPtr web_rust_crypto_parse_an_asn1_structure(ReadonlyBytes, Asn1Structure, bool, void*);
}

static void receive_key_usages(void* state, ReadonlySpan<KeyUsage> usages)
{
    *static_cast<Vector<KeyUsage>*>(state) = Vector<KeyUsage>(usages);
}

static Vector<KeyUsage> usage_intersection(ReadonlySpan<KeyUsage> a, ReadonlySpan<KeyUsage> b)
{
    Vector<KeyUsage> result;
    web_rust_crypto_usage_intersection(a, b, &result, receive_key_usages);
    return result;
}

Vector<KeyUsage> normalized_key_usages(ReadonlySpan<KeyUsage> usages)
{
    Vector<KeyUsage> result;
    web_rust_crypto_normalized_key_usages(usages, &result, receive_key_usages);
    return result;
}

static StringView key_usage_to_string(KeyUsage usage)
{
    switch (usage) {
    case KeyUsage::Encrypt:
        return "encrypt"sv;
    case KeyUsage::Decrypt:
        return "decrypt"sv;
    case KeyUsage::Sign:
        return "sign"sv;
    case KeyUsage::Verify:
        return "verify"sv;
    case KeyUsage::DeriveKey:
        return "deriveKey"sv;
    case KeyUsage::DeriveBits:
        return "deriveBits"sv;
    case KeyUsage::WrapKey:
        return "wrapKey"sv;
    case KeyUsage::UnwrapKey:
        return "unwrapKey"sv;
    case KeyUsage::EncapsulateKey:
        return "encapsulateKey"sv;
    case KeyUsage::EncapsulateBits:
        return "encapsulateBits"sv;
    case KeyUsage::DecapsulateKey:
        return "decapsulateKey"sv;
    case KeyUsage::DecapsulateBits:
        return "decapsulateBits"sv;
    }
    VERIFY_NOT_REACHED();
}

// Out of line to ensure this class has a key function
AlgorithmMethods::~AlgorithmMethods() = default;

int web_crypto_encode_base64url(ReadonlyBytes bytes, ReceiveEncodedBytes receive, void* state)
{
    auto encoded = encode_base64url(bytes, AK::OmitPadding::Yes);
    if (encoded.is_error())
        return encoded.error().code();
    receive(state, encoded.value().bytes_as_string_view().bytes());
    return 0;
}

static ErrorOr<Utf16String> base64_url_bytes_encode(ReadonlyBytes bytes)
{
    FlatPtr encoded = 0;
    if (auto error = web_rust_crypto_base64_url_bytes_encode(bytes, &encoded); error != 0)
        return Error::from_errno(error);
    return Utf16String::adopt_raw(encoded);
}

ErrorOr<Utf16String> base64_url_uint_encode(::Crypto::UnsignedBigInteger integer)
{
    // The representation of a positive or zero integer value as the
    // base64url encoding of the value's unsigned big-endian
    // representation as an octet sequence.  The octet sequence MUST
    // utilize the minimum number of octets needed to represent the
    // value.  Zero is represented as BASE64URL(single zero-valued
    // octet), which is "AA".

    auto bytes = TRY(ByteBuffer::create_uninitialized(integer.byte_length()));
    auto result = integer.export_data(bytes.span());
    return TRY(base64_url_bytes_encode(result));
}

Base64DecodeResult web_crypto_decode_base64url(FlatPtr raw, ByteBuffer* bytes, void* state, ReceiveEncodedBytes receive, FlatPtr* error_message)
{
    auto base64_url_string = Utf16String::from_raw(raw);
    auto base64_bytes_or_error = decode_base64url(base64_url_string.to_byte_string(), AK::LastChunkHandling::Loose);
    if (base64_bytes_or_error.is_error()) {
        if (base64_bytes_or_error.error().code() == ENOMEM)
            return Base64DecodeResult::OutOfMemory;
        *error_message = Utf16String::formatted("{}", base64_bytes_or_error.release_error()).into_raw();
        return Base64DecodeResult::InvalidBase64;
    }
    if (receive)
        receive(state, base64_bytes_or_error.value());
    else
        *bytes = base64_bytes_or_error.release_value();
    return Base64DecodeResult::Success;
}

WebIDL::ExceptionOr<ByteBuffer> base64_url_bytes_decode(JS::Realm& realm, Utf16String const& base64_url_string)
{
    auto& vm = realm.vm();
    ByteBuffer bytes;
    FlatPtr error_message = 0;
    switch (web_rust_crypto_base64_url_bytes_decode(base64_url_string.raw_identity(), &bytes, &error_message)) {
    case Base64DecodeResult::Success:
        return bytes;
    case Base64DecodeResult::NonAscii:
        return WebIDL::DataError::create(realm, "base64 decode: invalid base64url"_utf16);
    case Base64DecodeResult::OutOfMemory:
        return vm.throw_completion<JS::InternalError>(vm.error_message(::JS::VM::ErrorMessage::OutOfMemory));
    case Base64DecodeResult::InvalidBase64:
        return WebIDL::DataError::create(Utf16String::adopt_raw(error_message));
    }
    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<::Crypto::UnsignedBigInteger> base64_url_uint_decode(JS::Realm& realm, Utf16String const& base64_url_string)
{
    auto base64_bytes_be = TRY(base64_url_bytes_decode(realm, base64_url_string));
    return ::Crypto::UnsignedBigInteger::import_data(base64_bytes_be);
}

FlatPtr web_crypto_decode_asn1_structure(ReadonlyBytes data, Asn1Structure structure, void* out, bool* all_consumed)
{
    ::Crypto::ASN1::Decoder decoder(data);
    switch (structure) {
    case Asn1Structure::SubjectPublicKeyInfo: {
        auto subject_public_key = ::Crypto::Certificate::parse_subject_public_key_info(decoder);
        if (subject_public_key.is_error())
            return Utf16String::formatted("{}", subject_public_key.release_error()).into_raw();
        *static_cast<::Crypto::Certificate::SubjectPublicKey*>(out) = subject_public_key.release_value();
        break;
    }
    case Asn1Structure::PrivateKeyInfo: {
        auto private_key = ::Crypto::Certificate::parse_private_key_info(decoder);
        if (private_key.is_error())
            return Utf16String::formatted("{}", private_key.release_error()).into_raw();
        *static_cast<::Crypto::Certificate::PrivateKey*>(out) = private_key.release_value();
        break;
    }
    case Asn1Structure::OctetString: {
        auto octet_string = decoder.read<StringView>(::Crypto::ASN1::Class::Universal, ::Crypto::ASN1::Kind::OctetString);
        if (octet_string.is_error())
            return Utf16String::formatted("{}", octet_string.error()).into_raw();
        *static_cast<StringView*>(out) = octet_string.release_value();
        break;
    }
    }
    *all_consumed = decoder.eof();
    return 0;
}

template<typename Structure>
static WebIDL::ExceptionOr<Structure> parse_an_ASN1_structure(JS::Realm&, ReadonlyBytes data, bool exact_data = true)
{
    constexpr auto structure_kind = [] {
        if constexpr (IsSame<Structure, ::Crypto::Certificate::SubjectPublicKey>)
            return Asn1Structure::SubjectPublicKeyInfo;
        else if constexpr (IsSame<Structure, ::Crypto::Certificate::PrivateKey>)
            return Asn1Structure::PrivateKeyInfo;
        else if constexpr (IsSame<Structure, StringView>)
            return Asn1Structure::OctetString;
        else
            static_assert(DependentFalse<Structure>, "Don't know how to parse ASN.1 structure type");
    }();
    Structure structure;
    auto error = web_rust_crypto_parse_an_asn1_structure(data, structure_kind, exact_data, &structure);
    if (error != 0)
        return WebIDL::DataError::create(Utf16String::adopt_raw(error));
    return structure;
}

static WebIDL::ExceptionOr<::Crypto::Certificate::SubjectPublicKey> parse_a_subject_public_key_info(JS::Realm& realm, ReadonlyBytes bytes)
{
    return parse_an_ASN1_structure<::Crypto::Certificate::SubjectPublicKey>(realm, bytes);
}

static WebIDL::ExceptionOr<::Crypto::Certificate::PrivateKey> parse_a_private_key_info(JS::Realm& realm, ReadonlyBytes bytes)
{
    return parse_an_ASN1_structure<::Crypto::Certificate::PrivateKey>(realm, bytes);
}

static WebIDL::ExceptionOr<::Crypto::PK::RSAPrivateKey> parse_jwk_rsa_private_key(JS::Realm& realm, JsonWebKey const& jwk)
{
    auto n = TRY(base64_url_uint_decode(realm, *jwk.n));
    auto d = TRY(base64_url_uint_decode(realm, *jwk.d));
    auto e = TRY(base64_url_uint_decode(realm, *jwk.e));

    // We know that if any of the extra parameters are provided, all of them must be
    if (!jwk.p.has_value())
        return ::Crypto::PK::RSAPrivateKey(move(n), move(d), move(e));

    auto p = TRY(base64_url_uint_decode(realm, *jwk.p));
    auto q = TRY(base64_url_uint_decode(realm, *jwk.q));
    auto dp = TRY(base64_url_uint_decode(realm, *jwk.dp));
    auto dq = TRY(base64_url_uint_decode(realm, *jwk.dq));
    auto qi = TRY(base64_url_uint_decode(realm, *jwk.qi));

    return ::Crypto::PK::RSAPrivateKey(move(n), move(d), move(e), move(p), move(q), move(dp), move(dq), move(qi));
}

static WebIDL::ExceptionOr<::Crypto::PK::RSAPublicKey> parse_jwk_rsa_public_key(JS::Realm& realm, JsonWebKey const& jwk)
{
    auto e = TRY(base64_url_uint_decode(realm, *jwk.e));
    auto n = TRY(base64_url_uint_decode(realm, *jwk.n));

    return ::Crypto::PK::RSAPublicKey(move(n), move(e));
}

// https://www.rfc-editor.org/rfc/rfc7517#section-4.3
static WebIDL::ExceptionOr<void> validate_jwk_key_ops(JS::Realm&, JsonWebKey const& jwk, Vector<KeyUsage> const& usages)
{
    // Use of the "key_ops" member is OPTIONAL, unless the application requires its presence.
    if (!jwk.key_ops.has_value())
        return {};
    auto key_operations = *jwk.key_ops;

    // Duplicate key operation values MUST NOT be present in the array
    HashTable<Utf16String> seen_operations;
    for (auto const& key_operation : key_operations) {
        if (seen_operations.set(key_operation) != HashSetResult::InsertedNewEntry)
            return WebIDL::DataError::create(Utf16String::formatted("Duplicate key operation: {}", key_operation));
    }

    // Multiple unrelated key operations SHOULD NOT be specified for a key because of the potential
    // vulnerabilities associated with using the same key with multiple algorithms.  Thus, the
    // combinations "sign" with "verify", "encrypt" with "decrypt", and "wrapKey" with "unwrapKey"
    // are permitted, but other combinations SHOULD NOT be used.
    auto is_used_for_signing = seen_operations.contains("sign"_utf16) || seen_operations.contains("verify"_utf16);
    auto is_used_for_encryption = seen_operations.contains("encrypt"_utf16) || seen_operations.contains("decrypt"_utf16);
    auto is_used_for_wrapping = seen_operations.contains("wrapKey"_utf16) || seen_operations.contains("unwrapKey"_utf16);
    auto number_of_operation_types = is_used_for_signing + is_used_for_encryption + is_used_for_wrapping;
    if (number_of_operation_types > 1)
        return WebIDL::DataError::create("Multiple unrelated key operations are specified"_utf16);

    // The "use" and "key_ops" JWK members SHOULD NOT be used together; however, if both are used,
    // the information they convey MUST be consistent. Applications should specify which of these
    // members they use, if either is to be used by the application.
    if (jwk.use.has_value()) {
        for (auto const& key_operation : key_operations) {
            if (key_operation == "deriveKey"_utf16 || key_operation == "deriveBits"_utf16)
                continue;
            if (jwk.use == "sig"sv && key_operation != "sign"sv && key_operation != "verify"sv)
                return WebIDL::DataError::create("use=sig but key_ops does not contain 'sign' or 'verify'"_utf16);
            if (jwk.use == "enc"sv && (key_operation == "sign"sv || key_operation == "verify"sv))
                return WebIDL::DataError::create("use=enc but key_ops contains 'sign' or 'verify'"_utf16);
        }
    }

    // NOTE: This validation happens in multiple places in the spec, so it is here for convenience.
    for (auto const& usage : usages) {
        if (!seen_operations.contains(Utf16String::from_utf8(key_usage_to_string(usage))))
            return WebIDL::DataError::create(Utf16String::formatted("Missing key_ops usage: {}", key_usage_to_string(usage)));
    }

    return {};
}

AlgorithmParams::~AlgorithmParams() = default;

EcdhKeyDeriveParams::~EcdhKeyDeriveParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> EcdhKeyDeriveParams::from_value(JS::Realm& realm, JS::Value value)
{
    auto& vm = realm.vm();
    auto& object = value.as_object();

    auto key = TRY(crypto_key_from_value(vm, TRY(object.get("public"_utf16_fly_string))));

    return adopt_own<AlgorithmParams>(*new EcdhKeyDeriveParams { key });
}

Ed448Params::~Ed448Params() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> Ed448Params::from_value(JS::Realm& realm, JS::Value value)
{
    auto& vm = realm.vm();
    auto& object = value.as_object();

    auto maybe_context = Optional<ByteBuffer> {};
    if (TRY(object.has_property("context"_utf16_fly_string))) {
        auto context_value = TRY(object.get("context"_utf16_fly_string));
        if (!context_value.is_object() || !(is<JS::TypedArrayBase>(context_value.as_object()) || is<JS::ArrayBuffer>(context_value.as_object()) || is<JS::DataView>(context_value.as_object())))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "BufferSource");
        maybe_context = TRY_OR_THROW_OOM(vm, WebIDL::get_buffer_source_copy(context_value.as_object()));
    }

    return adopt_own<AlgorithmParams>(*new Ed448Params { maybe_context });
}

extern "C" {
void* web_crypto_asn1_allocate_key_info(Asn1Structure);
void web_crypto_asn1_delete_key_info(Asn1Structure, void*);
bool web_crypto_rsa_key_info_has_encryption_oid(Asn1Structure, void const*);
CryptoKey::InternalKeyData* web_crypto_rsa_key_info_read_key(Asn1Structure, void const*);
void web_crypto_delete_uninstalled_key(CryptoKey::InternalKeyData*);
bool web_crypto_rsa_key_is_valid(CryptoKey::InternalKeyData const*, bool*);
bool web_crypto_rsa_validate_public_exponent(ReadonlyBytes, u32);
bool web_crypto_rsa_validate_modulus_length(u32);
bool web_crypto_rsa_generate_key_pair(u32, ReadonlyBytes, CryptoKey::InternalKeyData**, CryptoKey::InternalKeyData**);
bool web_crypto_rsa_export_public_exponent(ReadonlyBytes, void*, ReceiveEncodedBytes);
bool web_crypto_rsa_read_key_metadata(CryptoKey::InternalKeyData const*, u32*, void*, ReceiveEncodedBytes);
JSCompletion web_crypto_rsa_parse_jwk_key(JS::Realm*, bool, ReadonlySpan<FlatPtr>, CryptoKey::InternalKeyData**);
bool web_crypto_rsa_export_der(CryptoKey::InternalKeyData const*, bool, void*, ReceiveEncodedBytes);
FlatPtr web_crypto_rsa_export_jwk_integer(CryptoKey::InternalKeyData const*, u8);
bool web_crypto_rsa_oaep_cipher(bool, ::Crypto::Hash::HashKind, CryptoKey::InternalKeyData const*, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool web_crypto_rsa_pss_sign(::Crypto::Hash::HashKind, CryptoKey::InternalKeyData const*, u32, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool web_crypto_rsa_pss_verify(::Crypto::Hash::HashKind, CryptoKey::InternalKeyData const*, u32, ReadonlyBytes, ReadonlyBytes, bool*);
bool web_crypto_rsa_pkcs1_sign(::Crypto::Hash::HashKind, CryptoKey::InternalKeyData const*, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool web_crypto_rsa_pkcs1_verify(::Crypto::Hash::HashKind, CryptoKey::InternalKeyData const*, ReadonlyBytes, ReadonlyBytes, bool*);
}

void* web_crypto_asn1_allocate_key_info(Asn1Structure structure)
{
    if (structure == Asn1Structure::SubjectPublicKeyInfo)
        return new ::Crypto::Certificate::SubjectPublicKey;
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    return new ::Crypto::Certificate::PrivateKey;
}

void web_crypto_asn1_delete_key_info(Asn1Structure structure, void* key_info)
{
    if (structure == Asn1Structure::SubjectPublicKeyInfo) {
        delete static_cast<::Crypto::Certificate::SubjectPublicKey*>(key_info);
        return;
    }
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    delete static_cast<::Crypto::Certificate::PrivateKey*>(key_info);
}

bool web_crypto_rsa_key_info_has_encryption_oid(Asn1Structure structure, void const* key_info)
{
    if (structure == Asn1Structure::SubjectPublicKeyInfo)
        return static_cast<::Crypto::Certificate::SubjectPublicKey const*>(key_info)->algorithm.identifier == ::Crypto::ASN1::rsa_encryption_oid;
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    return static_cast<::Crypto::Certificate::PrivateKey const*>(key_info)->algorithm.identifier == ::Crypto::ASN1::rsa_encryption_oid;
}

CryptoKey::InternalKeyData* web_crypto_rsa_key_info_read_key(Asn1Structure structure, void const* key_info)
{
    if (structure == Asn1Structure::SubjectPublicKeyInfo)
        return new CryptoKey::InternalKeyData(static_cast<::Crypto::Certificate::SubjectPublicKey const*>(key_info)->rsa);
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    return new CryptoKey::InternalKeyData(static_cast<::Crypto::Certificate::PrivateKey const*>(key_info)->rsa);
}

void web_crypto_delete_uninstalled_key(CryptoKey::InternalKeyData* key)
{
    delete key;
}

bool web_crypto_rsa_key_is_valid(CryptoKey::InternalKeyData const* key, bool* valid)
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

bool web_crypto_rsa_validate_public_exponent(ReadonlyBytes bytes, u32 modulus_length)
{
    auto public_exponent = ::Crypto::UnsignedBigInteger::import_data(bytes);
    return public_exponent >= ::Crypto::UnsignedBigInteger { 3 }
    && public_exponent.is_odd()
        && public_exponent.plus(1).one_based_index_of_highest_set_bit() <= modulus_length;
}

bool web_crypto_rsa_validate_modulus_length(u32 modulus_length)
{
    return !::Crypto::PK::RSA::validate_key_generation_parameters(modulus_length).is_error();
}

bool web_crypto_rsa_generate_key_pair(u32 modulus_length, ReadonlyBytes public_exponent, CryptoKey::InternalKeyData** out_public_key, CryptoKey::InternalKeyData** out_private_key)
{
    auto key_pair = ::Crypto::PK::RSA::generate_key_pair(modulus_length, ::Crypto::UnsignedBigInteger::import_data(public_exponent));
    if (key_pair.is_error())
        return false;
    *out_public_key = new CryptoKey::InternalKeyData(move(key_pair.value().public_key));
    *out_private_key = new CryptoKey::InternalKeyData(move(key_pair.value().private_key));
    return true;
}

static ErrorOr<ByteBuffer> export_rsa_exponent(::Crypto::UnsignedBigInteger const& exponent)
{
    auto bytes = TRY(ByteBuffer::create_uninitialized(exponent.byte_length()));
    return ByteBuffer::copy(exponent.export_data(bytes.span()));
}

bool web_crypto_rsa_export_public_exponent(ReadonlyBytes public_exponent, void* state, ReceiveEncodedBytes receive)
{
    auto bytes = export_rsa_exponent(::Crypto::UnsignedBigInteger::import_data(public_exponent));
    if (bytes.is_error())
        return false;
    receive(state, bytes.value());
    return true;
}

bool web_crypto_rsa_read_key_metadata(CryptoKey::InternalKeyData const* key, u32* modulus_length, void* state, ReceiveEncodedBytes receive)
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
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

JSCompletion web_crypto_rsa_parse_jwk_key(JS::Realm* realm, bool is_private, ReadonlySpan<FlatPtr> components, CryptoKey::InternalKeyData** out)
{
    auto component = [&](size_t index) -> Optional<Utf16String> {
        if (!components[index])
            return {};
        return Utf16String::from_raw(components[index]);
    };
    JsonWebKey jwk;
    jwk.n = component(0);
    jwk.e = component(1);
    jwk.d = component(2);
    jwk.p = component(3);
    jwk.q = component(4);
    jwk.dp = component(5);
    jwk.dq = component(6);
    jwk.qi = component(7);
    if (is_private) {
        auto result = parse_jwk_rsa_private_key(*realm, jwk);
        if (result.is_exception())
            return Bindings::completion_to_abi(*realm, result.release_error());
        *out = new CryptoKey::InternalKeyData(result.release_value());
    } else {
        auto result = parse_jwk_rsa_public_key(*realm, jwk);
        if (result.is_exception())
            return Bindings::completion_to_abi(*realm, result.release_error());
        *out = new CryptoKey::InternalKeyData(result.release_value());
    }
    return JS::HostABI::normal_completion_to_abi(0);
}

bool web_crypto_rsa_export_der(CryptoKey::InternalKeyData const* key, bool is_public, void* state, ReceiveEncodedBytes receive)
{
    auto result = is_public
        ? ::Crypto::PK::wrap_in_subject_public_key_info(key->get<::Crypto::PK::RSAPublicKey>(), ::Crypto::ASN1::rsa_encryption_oid, nullptr)
        : ::Crypto::PK::wrap_in_private_key_info(key->get<::Crypto::PK::RSAPrivateKey>(), ::Crypto::ASN1::rsa_encryption_oid, nullptr);
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

FlatPtr web_crypto_rsa_export_jwk_integer(CryptoKey::InternalKeyData const* key, u8 component)
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
    auto result = base64_url_uint_encode(integer);
    if (result.is_error())
        return 0;
    return result.release_value().into_raw();
}

bool web_crypto_rsa_oaep_cipher(bool encrypt, ::Crypto::Hash::HashKind hash_kind, CryptoKey::InternalKeyData const* key, ReadonlyBytes label, ReadonlyBytes input, void* state, ReceiveEncodedBytes receive)
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
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

bool web_crypto_rsa_pss_sign(::Crypto::Hash::HashKind hash_kind, CryptoKey::InternalKeyData const* key, u32 salt_length, ReadonlyBytes message, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::PK::RSA_PSS_EMSA rsa { hash_kind, key->get<::Crypto::PK::RSAPrivateKey>() };
    rsa.set_salt_length(salt_length);
    auto result = rsa.sign(message);
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

bool web_crypto_rsa_pss_verify(::Crypto::Hash::HashKind hash_kind, CryptoKey::InternalKeyData const* key, u32 salt_length, ReadonlyBytes message, ReadonlyBytes signature, bool* verified)
{
    ::Crypto::PK::RSA_PSS_EMSA rsa { hash_kind, key->get<::Crypto::PK::RSAPublicKey>() };
    rsa.set_salt_length(salt_length);
    auto result = rsa.verify(message, signature);
    if (result.is_error())
        return false;
    *verified = result.value();
    return true;
}

bool web_crypto_rsa_pkcs1_sign(::Crypto::Hash::HashKind hash_kind, CryptoKey::InternalKeyData const* key, ReadonlyBytes message, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::PK::RSA_PKCS1_EMSA rsa { hash_kind, key->get<::Crypto::PK::RSAPrivateKey>() };
    auto result = rsa.sign(message);
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

bool web_crypto_rsa_pkcs1_verify(::Crypto::Hash::HashKind hash_kind, CryptoKey::InternalKeyData const* key, ReadonlyBytes message, ReadonlyBytes signature, bool* verified)
{
    ::Crypto::PK::RSA_PKCS1_EMSA rsa { hash_kind, key->get<::Crypto::PK::RSAPublicKey>() };
    auto result = rsa.verify(message, signature);
    if (result.is_error())
        return false;
    *verified = result.value();
    return true;
}

extern "C" {
bool web_crypto_aes_cbc_cipher(bool, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool web_crypto_aes_ctr_cipher(bool, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool web_crypto_aes_kw_cipher(bool, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool web_crypto_aes_gcm_encrypt(ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, size_t, void*, void*, ReceiveEncodedBytes);
bool web_crypto_aes_gcm_decrypt(ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
FlatPtr web_crypto_aes_ocb_cipher(bool, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, size_t, void*, ReceiveEncodedBytes);
}

bool web_crypto_aes_cbc_cipher(bool encrypt, ReadonlyBytes key, ReadonlyBytes input, ReadonlyBytes iv, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Cipher::AESCBCCipher cipher(key);
    auto result = encrypt ? cipher.encrypt(input, iv) : cipher.decrypt(input, iv);
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

bool web_crypto_aes_ctr_cipher(bool encrypt, ReadonlyBytes key, ReadonlyBytes input, ReadonlyBytes counter, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Cipher::AESCTRCipher cipher(key);
    auto result = encrypt ? cipher.encrypt(input, counter) : cipher.decrypt(input, counter);
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

bool web_crypto_aes_kw_cipher(bool wrap, ReadonlyBytes key, ReadonlyBytes input, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Cipher::AESKWCipher cipher(key);
    auto result = wrap ? cipher.wrap(input) : cipher.unwrap(input);
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

bool web_crypto_aes_gcm_encrypt(ReadonlyBytes key, ReadonlyBytes plaintext, ReadonlyBytes iv, ReadonlyBytes additional_data, size_t tag_length, void* ciphertext, void* tag, ReceiveEncodedBytes receive)
{
    ::Crypto::Cipher::AESGCMCipher cipher(key);
    auto result = cipher.encrypt(plaintext, iv, additional_data, tag_length);
    if (result.is_error())
        return false;
    receive(ciphertext, result.value().ciphertext);
    receive(tag, result.value().tag);
    return true;
}

bool web_crypto_aes_gcm_decrypt(ReadonlyBytes key, ReadonlyBytes ciphertext, ReadonlyBytes iv, ReadonlyBytes additional_data, ReadonlyBytes tag, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Cipher::AESGCMCipher cipher(key);
    auto result = cipher.decrypt(ciphertext, iv, additional_data, tag);
    if (result.is_error()) {
        dbgln("FAILED: {}", result.error());
        return false;
    }
    receive(state, result.value());
    return true;
}

FlatPtr web_crypto_aes_ocb_cipher(bool encrypt, ReadonlyBytes key, ReadonlyBytes input, ReadonlyBytes iv, ReadonlyBytes additional_data, size_t tag_length, void* state, ReceiveEncodedBytes receive)
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

extern "C" bool web_crypto_sha_digest(::Crypto::Hash::HashKind, ReadonlyBytes, void*, ReceiveEncodedBytes);

bool web_crypto_sha_digest(::Crypto::Hash::HashKind hash_kind, ReadonlyBytes data, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Hash::Manager hash { hash_kind };
    hash.update(data);

    auto digest = hash.digest();
    auto result_buffer = ByteBuffer::copy(digest.immutable_data(), hash.digest_size());
    if (result_buffer.is_error())
        return false;

    receive(state, result_buffer.value());
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
bool web_crypto_ec_generate_key_pair(EcCurve, CryptoKey::InternalKeyData**, CryptoKey::InternalKeyData**);
u8 web_crypto_ec_sign(EcCurve, CryptoKey::InternalKeyData const*, ReadonlyBytes, FlatPtr*, void*, void*, ReceiveEncodedBytes);
FlatPtr web_crypto_ec_verify(EcCurve, CryptoKey::InternalKeyData const*, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, bool*);
size_t web_crypto_ec_public_key_scalar_size(CryptoKey::InternalKeyData const*);
u8 web_crypto_ec_compute_coordinate(EcCurve, CryptoKey::InternalKeyData const*, CryptoKey::InternalKeyData const*, FlatPtr*, void*, ReceiveEncodedBytes);
bool web_crypto_ec_key_info_has_public_key_oid(Asn1Structure, void const*);
bool web_crypto_ec_key_info_parameters(Asn1Structure, void const*, ReadonlySpan<u32>*);
bool web_crypto_ec_private_key_info_parameters_match(void const*);
CryptoKey::InternalKeyData* web_crypto_ec_key_info_read_key(Asn1Structure, void const*);
CryptoKey::InternalKeyData* web_crypto_ec_key_from_coordinates(ReadonlyBytes, ReadonlyBytes, size_t, OptionalBytes);
CryptoKey::InternalKeyData* web_crypto_ec_parse_raw_public_key(ReadonlyBytes);
bool web_crypto_ec_key_is_valid(EcCurve, CryptoKey::InternalKeyData const*, bool*);
bool web_crypto_ec_export_der(EcCurve, CryptoKey::InternalKeyData const*, bool, void*, ReceiveEncodedBytes);
bool web_crypto_ec_export_uncompressed(CryptoKey::InternalKeyData const*, void*, ReceiveEncodedBytes);
bool web_crypto_ec_export_public_coordinates(EcCurve, CryptoKey::InternalKeyData const*, void*, void*, ReceiveEncodedBytes);
bool web_crypto_ec_export_private_scalar(CryptoKey::InternalKeyData const*, void*, ReceiveEncodedBytes);
}

bool web_crypto_ec_generate_key_pair(EcCurve curve_id, CryptoKey::InternalKeyData** out_public_key, CryptoKey::InternalKeyData** out_private_key)
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
    *out_public_key = new CryptoKey::InternalKeyData(move(public_key));
    *out_private_key = new CryptoKey::InternalKeyData(move(private_key));
    return true;
}

u8 web_crypto_ec_sign(EcCurve curve_id, CryptoKey::InternalKeyData const* key, ReadonlyBytes digest, FlatPtr* error, void* r, void* s, ReceiveEncodedBytes receive)
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

FlatPtr web_crypto_ec_verify(EcCurve curve_id, CryptoKey::InternalKeyData const* key, ReadonlyBytes digest, ReadonlyBytes r_bytes, ReadonlyBytes s_bytes, bool* verified)
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

size_t web_crypto_ec_public_key_scalar_size(CryptoKey::InternalKeyData const* key)
{
    return key->get<::Crypto::PK::ECPublicKey>().scalar_size();
}

u8 web_crypto_ec_compute_coordinate(EcCurve curve_id, CryptoKey::InternalKeyData const* key, CryptoKey::InternalKeyData const* public_key, FlatPtr* error, void* state, ReceiveEncodedBytes receive)
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
        return static_cast<::Crypto::Certificate::SubjectPublicKey const*>(key_info)->algorithm;
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    return static_cast<::Crypto::Certificate::PrivateKey const*>(key_info)->algorithm;
}

bool web_crypto_ec_key_info_has_public_key_oid(Asn1Structure structure, void const* key_info)
{
    return key_info_algorithm(structure, key_info).identifier == ::Crypto::ASN1::ec_public_key_encryption_oid;
}

bool web_crypto_ec_key_info_parameters(Asn1Structure structure, void const* key_info, ReadonlySpan<u32>* parameters)
{
    auto const& algorithm = key_info_algorithm(structure, key_info);
    if (!algorithm.ec_parameters.has_value())
        return false;
    *parameters = algorithm.ec_parameters->span();
    return true;
}

bool web_crypto_ec_private_key_info_parameters_match(void const* key_info)
{
    auto const& private_key_info = *static_cast<::Crypto::Certificate::PrivateKey const*>(key_info);
    auto const& parameters = private_key_info.ec.parameters();
    return !parameters.has_value() || *parameters == private_key_info.algorithm.ec_parameters.value_or({});
}

CryptoKey::InternalKeyData* web_crypto_ec_key_info_read_key(Asn1Structure structure, void const* key_info)
{
    if (structure == Asn1Structure::SubjectPublicKeyInfo)
        return new CryptoKey::InternalKeyData(static_cast<::Crypto::Certificate::SubjectPublicKey const*>(key_info)->ec);
    VERIFY(structure == Asn1Structure::PrivateKeyInfo);
    return new CryptoKey::InternalKeyData(static_cast<::Crypto::Certificate::PrivateKey const*>(key_info)->ec);
}

CryptoKey::InternalKeyData* web_crypto_ec_key_from_coordinates(ReadonlyBytes x, ReadonlyBytes y, size_t coord_size, OptionalBytes d)
{
    auto public_key = ::Crypto::PK::ECPublicKey { ::Crypto::UnsignedBigInteger::import_data(x), ::Crypto::UnsignedBigInteger::import_data(y), coord_size };
    if (!d.has_value)
        return new CryptoKey::InternalKeyData(move(public_key));
    return new CryptoKey::InternalKeyData(::Crypto::PK::ECPrivateKey { ::Crypto::UnsignedBigInteger::import_data(d.bytes), coord_size, {}, move(public_key) });
}

CryptoKey::InternalKeyData* web_crypto_ec_parse_raw_public_key(ReadonlyBytes data)
{
    auto result = ::Crypto::PK::EC::parse_ec_key(data, false, {});
    if (result.is_error())
        return nullptr;
    return new CryptoKey::InternalKeyData(move(result.value().public_key));
}

bool web_crypto_ec_key_is_valid(EcCurve curve_id, CryptoKey::InternalKeyData const* key, bool* valid)
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

bool web_crypto_ec_export_der(EcCurve curve, CryptoKey::InternalKeyData const* key, bool is_public, void* state, ReceiveEncodedBytes receive)
{
    auto result = [&]() -> ErrorOr<ByteBuffer> {
        auto ec_params = ec_curve_oid(curve);
        if (is_public) {
            auto public_key_bytes = TRY(key->get<::Crypto::PK::ECPublicKey>().to_uncompressed());
            return ::Crypto::PK::wrap_in_subject_public_key_info(public_key_bytes, ::Crypto::ASN1::ec_public_key_encryption_oid, ec_params);
        }
        return ::Crypto::PK::wrap_in_private_key_info(key->get<::Crypto::PK::ECPrivateKey>(), ::Crypto::ASN1::ec_public_key_encryption_oid, ec_params);
    }();
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

bool web_crypto_ec_export_uncompressed(CryptoKey::InternalKeyData const* key, void* state, ReceiveEncodedBytes receive)
{
    auto result = key->get<::Crypto::PK::ECPublicKey>().to_uncompressed();
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

bool web_crypto_ec_export_public_coordinates(EcCurve curve_id, CryptoKey::InternalKeyData const* key, void* x, void* y, ReceiveEncodedBytes receive)
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

bool web_crypto_ec_export_private_scalar(CryptoKey::InternalKeyData const* key, void* state, ReceiveEncodedBytes receive)
{
    auto result = key->get<::Crypto::PK::ECPrivateKey>().d_bytes();
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

// https://w3c.github.io/webcrypto/#ed25519-operations-generate-key
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> ED25519::generate_key(JS::Realm&, [[maybe_unused]] AlgorithmParams const& params, bool extractable, Vector<KeyUsage> const& key_usages)
{
    // 1. If usages contains an entry which is not one of "sign" or "verify", then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != KeyUsage::Sign && usage != KeyUsage::Verify) {
            return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
        }
    }

    // 2. Generate an Ed25519 key pair, as defined in [RFC8032], section 5.1.5.
    ::Crypto::Curves::Ed25519 curve;
    auto maybe_private_key = curve.generate_private_key();
    if (maybe_private_key.is_error())
        return WebIDL::OperationError::create("Failed to generate private key"_utf16);
    auto private_key_data = maybe_private_key.release_value();

    auto maybe_public_key = curve.generate_public_key(private_key_data);
    if (maybe_public_key.is_error())
        return WebIDL::OperationError::create("Failed to generate public key"_utf16);
    auto public_key_data = maybe_public_key.release_value();

    // 5. Let publicKey be a new CryptoKey associated with the relevant global object of this [HTML],
    // and representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(public_key_data) } });

    // 6. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(KeyType::Public);

    // 3. Let algorithm be a new KeyAlgorithm object.
    // 4. Set the name attribute of algorithm to "Ed25519".
    // 7. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(CryptoKey::KeyAlgorithmData { "Ed25519"_utf16 });

    // 8. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 9. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
    public_key->set_usages(usage_intersection(key_usages, { { KeyUsage::Verify } }));

    // 10. Let privateKey be a new CryptoKey associated with the relevant global object of this [HTML],
    // and representing the private key of the generated key pair.
    auto private_key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPrivateKey { move(private_key_data) } });

    // 11. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(KeyType::Private);

    // 12. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(CryptoKey::KeyAlgorithmData { "Ed25519"_utf16 });

    // 13. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 14. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
    private_key->set_usages(usage_intersection(key_usages, { { KeyUsage::Sign } }));

    // 15. Let result be a new CryptoKeyPair dictionary.
    // 16. Set the publicKey attribute of result to be publicKey.
    // 17. Set the privateKey attribute of result to be privateKey.
    // 18. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
    return Variant<GC::Ref<CryptoKey>, CryptoKeyPair> { CryptoKeyPair { public_key, private_key } };
}

// https://w3c.github.io/webcrypto/#ed25519-operations-import-key
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> ED25519::import_key(JS::Realm& realm,
    [[maybe_unused]] Web::Crypto::AlgorithmParams const& params,
    KeyFormat format,
    CryptoKey::ImportKeyData key_data,
    bool extractable,
    Vector<KeyUsage> const& usages)
{
    GC::Ptr<CryptoKey> key = nullptr;

    // 1. Let keyData be the key data to be imported.

    // 2. If format is "spki":
    if (format == KeyFormat::Spki) {
        // 1. If usages contains an entry which is not "verify" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != KeyUsage::Verify) {
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
            }
        }

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
        //    is not equal to the id-Ed25519 object identifier defined in [RFC8410], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::ed25519_oid)
            return WebIDL::DataError::create("Invalid algorithm identifier"_utf16);

        // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
        if (spki.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create("Invalid algorithm parameters"_utf16);

        // 6. Let publicKey be the Ed25519 public key identified by the subjectPublicKey field of spki.
        auto const& public_key = spki.raw_key;

        // 7. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
        //    and that represents publicKey.
        key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(public_key) } });

        // 8. Set the [[type]] internal slot of key to "public"
        key->set_type(KeyType::Public);

        // 9. Let algorithm be a new KeyAlgorithm.
        // 10. Set the name attribute of algorithm to "Ed25519".
        // 11. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "Ed25519"_utf16 });
    }

    // 2. If format is "pkcs8":
    else if (format == KeyFormat::Pkcs8) {
        // 1. If usages contains an entry which is not "sign" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != KeyUsage::Sign) {
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
            }
        }

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurs while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field
        //    of privateKeyInfo is not equal to the id-Ed25519 object identifier defined in [RFC8410], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::ed25519_oid)
            return WebIDL::DataError::create("Invalid algorithm identifier"_utf16);

        // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo is present,
        //    then throw a DataError.
        if (private_key_info.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create("Invalid algorithm parameters"_utf16);

        // 6. Let curvePrivateKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 CurvePrivateKey structure
        //    specified in Section 7 of [RFC8410], and exactData set to true.
        // 7. If an error occurred while parsing, then throw a DataError.
        auto curve_private_key = TRY(parse_an_ASN1_structure<StringView>(realm, private_key_info.raw_key, true));
        auto curve_private_key_bytes = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::copy(curve_private_key.bytes()));

        // 8. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
        //    and that represents the Ed25519 private key identified by curvePrivateKey.
        key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPrivateKey { move(curve_private_key_bytes) } });

        // 9. Set the [[type]] internal slot of key to "private"
        key->set_type(KeyType::Private);

        // 10. Let algorithm be a new KeyAlgorithm.
        // 11. Set the name attribute of algorithm to "Ed25519".
        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "Ed25519"_utf16 });
    }

    // 2. If format is "jwk":
    else if (format == KeyFormat::Jwk) {
        // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
        //    Otherwise: Throw a DataError.
        if (!key_data.has<JsonWebKey>())
            return WebIDL::DataError::create("keyData is not a JsonWebKey dictionary"_utf16);
        auto& jwk = key_data.get<JsonWebKey>();

        // 2. If the d field is present and usages contains a value which is not "sign",
        //    or, if the d field is not present and usages contains a value which is not "verify" then throw a SyntaxError.
        if (jwk.d.has_value()) {
            for (auto const& usage : usages) {
                if (usage != KeyUsage::Sign) {
                    return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
                }
            }
        } else {
            for (auto const& usage : usages) {
                if (usage != KeyUsage::Verify) {
                    return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
                }
            }
        }

        // 3. If the kty field of jwk is not "OKP", then throw a DataError.
        if (jwk.kty != "OKP"sv)
            return WebIDL::DataError::create("Invalid key type"_utf16);

        // 4. If the crv field of jwk is not "Ed25519", then throw a DataError.
        if (jwk.crv != "Ed25519"sv)
            return WebIDL::DataError::create("Invalid curve"_utf16);

        // 5. If the alg field of jwk is present and is not "Ed25519" or "EdDSA", then throw a DataError.
        if (jwk.alg.has_value() && !jwk.alg.value().is_one_of("Ed25519"sv, "EdDSA"sv))
            return WebIDL::DataError::create("Invalid algorithm"_utf16);

        // 6. If usages is non-empty and the use field of jwk is present and is not "sig", then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && jwk.use.value() != "sig"_utf16)
            return WebIDL::DataError::create("Invalid key usage"_utf16);

        // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
        //    or it does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(realm, jwk, usages));

        // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !jwk.ext.value() && extractable)
            return WebIDL::DataError::create("Invalid extractable"_utf16);

        // 9. If the d field is present:
        if (jwk.d.has_value()) {
            // 1. If jwk does not meet the requirements of the JWK private key format described in Section 2 of [RFC8037],
            //    then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create("Invalid key type"_utf16);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "Ed25519"sv)
                return WebIDL::DataError::create("Invalid curve"_utf16);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create("Missing x field"_utf16);
            auto public_key = TRY(base64_url_bytes_decode(realm, jwk.x.value()));
            if (public_key.size() != 32)
                return WebIDL::DataError::create("Invalid key length"_utf16);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (!jwk.d.has_value())
                return WebIDL::DataError::create("Present d field"_utf16);
            auto private_key = TRY(base64_url_bytes_decode(realm, jwk.d.value()));
            if (private_key.size() != 32)
                return WebIDL::DataError::create("Invalid key length"_utf16);

            ::Crypto::Curves::Ed25519 curve;
            auto derived_public_key = TRY_OR_THROW_OOM(realm.vm(), curve.generate_public_key(private_key));
            if (derived_public_key != public_key)
                return WebIDL::DataError::create("Invalid key pair"_utf16);

            // 2. Let key be a new CryptoKey object that represents the Ed25519 private key identified by interpreting jwk according to Section 2 of [RFC8037].
            key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPrivateKey { move(private_key) } });

            // 3. Set the [[type]] internal slot of Key to "private".
            key->set_type(KeyType::Private);
        }
        // Otherwise:
        else {
            // 1. If jwk does not meet the requirements of the JWK public key format described in Section 2 of [RFC8037], then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create("Invalid key type"_utf16);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "Ed25519"sv)
                return WebIDL::DataError::create("Invalid curve"_utf16);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create("Missing x field"_utf16);
            auto public_key = TRY(base64_url_bytes_decode(realm, jwk.x.value()));
            if (public_key.size() != 32)
                return WebIDL::DataError::create("Invalid key length"_utf16);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (jwk.d.has_value())
                return WebIDL::DataError::create("Present d field"_utf16);

            // 2. Let key be a new CryptoKey object that represents the Ed25519 public key identified by interpreting jwk according to Section 2 of [RFC8037].
            key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(public_key) } });

            // 3. Set the [[type]] internal slot of Key to "public".
            key->set_type(KeyType::Public);
        }

        // 10. Let algorithm be a new instance of a KeyAlgorithm object.
        // 11. Set the name attribute of algorithm to "Ed25519".
        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "Ed25519"_utf16 });
    }

    // 2. If format is "raw":
    // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
    // For all existing asymmetric algorithms in WebCrypto, "raw-public" acts as an alias of "raw".
    else if (format == KeyFormat::Raw || format == KeyFormat::RawPublic) {
        // 1. If usages contains an entry which is not "verify" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != KeyUsage::Verify) {
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
            }
        }

        // 2. If the length in bits of keyData is not 256 then throw a DataError.
        auto& data = key_data.get<ByteBuffer>();
        if (data.size() * 8 != 256)
            return WebIDL::DataError::create("Invalid key length"_utf16);

        // 5. Let key be a new CryptoKey representing the key data provided in keyData.
        key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(data) } });

        // 6. Set the [[type]] internal slot of key to "public"
        key->set_type(KeyType::Public);

        // 3. Let algorithm be a new KeyAlgorithm object.
        // 4. Set the name attribute of algorithm to "Ed25519".
        // 7. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "Ed25519"_utf16 });
    }

    // 2. Otherwise:
    else {
        // throw a NotSupportedError.
        return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
    }

    // 3. Return key.
    return GC::Ref { *key };
}

// https://w3c.github.io/webcrypto/#ed25519-operations-export-key
WebIDL::ExceptionOr<ExportKeyResult> ED25519::export_key(JS::Realm& realm, KeyFormat format, GC::Ref<CryptoKey> key)
{
    auto& vm = realm.vm();

    // 1. Let key be the CryptoKey to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    // 3. If format is "spki":
    if (format == KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != KeyType::Public)
            return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //      * Set the algorithm object identifier to the id-Ed25519 OID defined in [RFC8410].
        //    * Set the subjectPublicKey field to keyData.
        auto ed25519_oid = ::Crypto::ASN1::ed25519_oid;
        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_subject_public_key_info(handle.get<OKPPublicKey>().bytes, ed25519_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return ExportKeyResult { move(data) };
    }

    // 3. If format is "pkcs8":
    if (format == KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != KeyType::Private)
            return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

        // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
        //    * Set the version field to 0.
        //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        //      * Set the algorithm object identifier to the id-Ed25519 OID defined in [RFC8410].
        //    * Set the privateKey field to the result of DER-encoding a CurvePrivateKey ASN.1 type,
        //      as defined in Section 7 of [RFC8410], that represents the Ed25519 private key
        //      represented by the [[handle]] internal slot of key
        ::Crypto::ASN1::Encoder encoder;
        TRY_OR_THROW_OOM(vm, encoder.write(handle.get<OKPPrivateKey>().bytes.bytes()));

        auto ed25519_oid = ::Crypto::ASN1::ed25519_oid;
        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_private_key_info(encoder.finish(), ed25519_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return ExportKeyResult { move(data) };
    }

    // 2. If format is "jwk":
    if (format == KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        JsonWebKey jwk;

        // 2. Set the kty attribute of jwk to "OKP".
        jwk.kty = "OKP"_utf16;

        // 3. Set the alg attribute of jwk to "Ed25519".
        jwk.alg = "Ed25519"_utf16;

        // 4. Set the crv attribute of jwk to "Ed25519".
        jwk.crv = "Ed25519"_utf16;

        // 5. Set the x attribute of jwk according to the definition in Section 2 of [RFC8037].
        if (key->type() == KeyType::Public) {
            jwk.x = Utf16String::from_utf8(TRY_OR_THROW_OOM(vm, encode_base64url(handle.get<OKPPublicKey>().bytes, AK::OmitPadding::Yes)));
        } else {
            // The "x" parameter of the "epk" field is set as follows:
            // Apply the appropriate ECDH function to the ephemeral private key (as scalar input)
            // and the standard base point (as u-coordinate input).
            // The base64url encoding of the output is the value for the "x" parameter of the "epk" field.
            ::Crypto::Curves::Ed25519 curve;
            auto public_key = TRY_OR_THROW_OOM(vm, curve.generate_public_key(handle.get<OKPPrivateKey>().bytes));
            jwk.x = TRY_OR_THROW_OOM(vm, base64_url_bytes_encode(public_key));
        }

        // 6. If the [[type]] internal slot of key is "private"
        if (key->type() == KeyType::Private) {
            // 1. Set the d attribute of jwk according to the definition in Section 2 of [RFC8037].
            jwk.d = TRY_OR_THROW_OOM(vm, base64_url_bytes_encode(handle.get<OKPPrivateKey>().bytes));
        }

        // 7. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<Utf16String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages())
            jwk.key_ops->unchecked_append(Utf16String::from_utf8(key_usage_to_string(usage)));

        // 8. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 9. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        return ExportKeyResult { move(jwk) };
    }

    // 2. If format is "raw":
    // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
    // For all existing asymmetric algorithms in WebCrypto, "raw-public" acts as an alias of "raw".
    if (format == KeyFormat::Raw || format == KeyFormat::RawPublic) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != KeyType::Public)
            return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

        // 2. Let data be an octet string representing the Ed25519 public key represented by the [[handle]] internal slot of key.
        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return ExportKeyResult { handle.get<OKPPublicKey>().bytes };
    }

    // 2. Otherwise:
    //    throw a NotSupportedError.
    return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
}

Optional<ReadonlySpan<KeyUsage>> ED25519::public_key_usages() const
{
    static constexpr Array usages { KeyUsage::Verify };
    return usages.span();
}

// https://w3c.github.io/webcrypto/#ed25519-operations-sign
WebIDL::ExceptionOr<ByteBuffer> ED25519::sign(JS::Realm& realm, [[maybe_unused]] AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& message)
{
    auto& vm = realm.vm();

    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != KeyType::Private)
        return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

    // 2. Perform the Ed25519 signing process, as specified in [RFC8032], Section 5.1.6,
    // with message as M, using the Ed25519 private key associated with key.
    auto const& private_key = key->handle().get<OKPPrivateKey>().bytes;

    ::Crypto::Curves::Ed25519 curve;
    auto maybe_public_key = curve.generate_public_key(private_key);
    if (maybe_public_key.is_error())
        return WebIDL::OperationError::create("Failed to generate public key"_utf16);
    auto public_key = maybe_public_key.release_value();

    auto maybe_signature = curve.sign(private_key, message);
    if (maybe_signature.is_error())
        return WebIDL::OperationError::create("Failed to sign message"_utf16);
    auto signature = maybe_signature.release_value();

    // 3. Return a new ArrayBuffer associated with the relevant global object of this [HTML],
    // and containing the bytes of the signature resulting from performing the Ed25519 signing process.
    auto result = TRY_OR_THROW_OOM(vm, ByteBuffer::copy(signature));
    return result;
}

// https://w3c.github.io/webcrypto/#ed25519-operations-verify
WebIDL::ExceptionOr<bool> ED25519::verify(JS::Realm&, [[maybe_unused]] AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& signature, ByteBuffer const& message)
{
    // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
    if (key->type() != KeyType::Public)
        return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

    // NOTE: this is checked by ED25519::verify().
    // 2. If the key data of key represents an invalid point or a small-order element on the Elliptic Curve of Ed25519, return false.
    // 3. If the point R, encoded in the first half of signature, represents an invalid point or a small-order element on the Elliptic Curve of Ed25519, return false.

    // 4. Perform the Ed25519 verification steps, as specified in [RFC8032], Section 5.1.7,
    // using the cofactorless (unbatched) equation, [S]B = R + [k]A', on the signature,
    // with message as M, using the Ed25519 public key associated with key.

    // 9. Let result be a boolean with the value true if the signature is valid and the value false otherwise.
    ::Crypto::Curves::Ed25519 curve;
    auto maybe_verified = curve.verify(key->handle().get<OKPPublicKey>().bytes, signature, message);
    if (maybe_verified.is_error())
        return WebIDL::OperationError::create(Utf16String::from_utf8(maybe_verified.error().string_literal()));

    // 10. Return result.
    return maybe_verified.release_value();
}

// https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> ED448::generate_key(JS::Realm&, [[maybe_unused]] AlgorithmParams const& params, bool extractable, Vector<KeyUsage> const& key_usages)
{
    // 1. If usages contains a value which is not one of "sign" or "verify", then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != KeyUsage::Sign && usage != KeyUsage::Verify) {
            return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
        }
    }

    // 2. Generate an Ed448 key pair, as defined in [RFC8032], section 5.1.5.
    ::Crypto::Curves::Ed448 curve;
    auto maybe_private_key = curve.generate_private_key();
    if (maybe_private_key.is_error())
        return WebIDL::OperationError::create("Failed to generate private key"_utf16);
    auto private_key_data = maybe_private_key.release_value();

    auto maybe_public_key = curve.generate_public_key(private_key_data);
    if (maybe_public_key.is_error())
        return WebIDL::OperationError::create("Failed to generate public key"_utf16);
    auto public_key_data = maybe_public_key.release_value();

    // 5. Let publicKey be a new CryptoKey associated with the relevant global object of this [HTML],
    // and representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(public_key_data) } });

    // 6. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(KeyType::Public);

    // 3. Let algorithm be a new KeyAlgorithm object.
    // 4. Set the name attribute of algorithm to "Ed448".
    // 7. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(CryptoKey::KeyAlgorithmData { "Ed448"_utf16 });

    // 8. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 9. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
    public_key->set_usages(usage_intersection(key_usages, { { KeyUsage::Verify } }));

    // 10. Let privateKey be a new CryptoKey associated with the relevant global object of this [HTML],
    // and representing the private key of the generated key pair.
    auto private_key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPrivateKey { move(private_key_data) } });

    // 11. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(KeyType::Private);

    // 12. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(CryptoKey::KeyAlgorithmData { "Ed448"_utf16 });

    // 13. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 14. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
    private_key->set_usages(usage_intersection(key_usages, { { KeyUsage::Sign } }));

    // 15. Let result be a new CryptoKeyPair dictionary.
    // 16. Set the publicKey attribute of result to be publicKey.
    // 17. Set the privateKey attribute of result to be privateKey.
    // 18. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
    return Variant<GC::Ref<CryptoKey>, CryptoKeyPair> { CryptoKeyPair { public_key, private_key } };
}

// https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> ED448::import_key(JS::Realm& realm,
    [[maybe_unused]] Web::Crypto::AlgorithmParams const& params,
    KeyFormat format,
    CryptoKey::ImportKeyData key_data,
    bool extractable,
    Vector<KeyUsage> const& usages)
{
    GC::Ptr<CryptoKey> key = nullptr;

    // 1. Let keyData be the key data to be imported.

    // 2. If format is "spki":
    if (format == KeyFormat::Spki) {
        // 1. If usages contains a value which is not "verify" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != KeyUsage::Verify) {
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
            }
        }

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
        //    is not equal to the id-Ed448 object identifier defined in [RFC8410], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::ed448_oid)
            return WebIDL::DataError::create("Invalid algorithm identifier"_utf16);

        // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
        if (spki.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create("Invalid algorithm parameters"_utf16);

        // 6. Let publicKey be the Ed448 public key identified by the subjectPublicKey field of spki.
        auto const& public_key = spki.raw_key;

        // 7. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
        //    and that represents publicKey.
        key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(public_key) } });

        // 8. Set the [[type]] internal slot of key to "public"
        key->set_type(KeyType::Public);

        // 9. Let algorithm be a new KeyAlgorithm.
        // 10. Set the name attribute of algorithm to "Ed448".
        // 11. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "Ed448"_utf16 });
    }

    // 2. If format is "pkcs8":
    else if (format == KeyFormat::Pkcs8) {
        // 1. If usages contains a value which is not "sign" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != KeyUsage::Sign) {
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
            }
        }

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurs while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field
        //    of privateKeyInfo is not equal to the id-Ed448 object identifier defined in [RFC8410], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::ed448_oid)
            return WebIDL::DataError::create("Invalid algorithm identifier"_utf16);

        // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo is present,
        //    then throw a DataError.
        if (private_key_info.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create("Invalid algorithm parameters"_utf16);

        // 6. Let curvePrivateKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 CurvePrivateKey structure
        //    specified in Section 7 of [RFC8410], and exactData set to true.
        // 7. If an error occurred while parsing, then throw a DataError.
        auto curve_private_key = TRY(parse_an_ASN1_structure<StringView>(realm, private_key_info.raw_key, true));
        auto curve_private_key_bytes = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::copy(curve_private_key.bytes()));

        // 8. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
        //    and that represents the Ed448 private key identified by curvePrivateKey.
        key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPrivateKey { move(curve_private_key_bytes) } });

        // 9. Set the [[type]] internal slot of key to "private"
        key->set_type(KeyType::Private);

        // 10. Let algorithm be a new KeyAlgorithm.
        // 11. Set the name attribute of algorithm to "Ed448".
        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "Ed448"_utf16 });
    }

    // 2. If format is "jwk":
    else if (format == KeyFormat::Jwk) {
        // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
        //    Otherwise: Throw a DataError.
        if (!key_data.has<JsonWebKey>())
            return WebIDL::DataError::create("keyData is not a JsonWebKey dictionary"_utf16);
        auto& jwk = key_data.get<JsonWebKey>();

        // 2. If the d field is present and usages contains a value which is not "sign",
        //    or, if the d field is not present and usages contains a value which is not "verify" then throw a SyntaxError.
        if (jwk.d.has_value()) {
            for (auto const& usage : usages) {
                if (usage != KeyUsage::Sign) {
                    return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
                }
            }
        } else {
            for (auto const& usage : usages) {
                if (usage != KeyUsage::Verify) {
                    return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
                }
            }
        }

        // 3. If the kty field of jwk is not "OKP", then throw a DataError.
        if (jwk.kty != "OKP"sv)
            return WebIDL::DataError::create("Invalid key type"_utf16);

        // 4. If the crv field of jwk is not "Ed448", then throw a DataError.
        if (jwk.crv != "Ed448"sv)
            return WebIDL::DataError::create("Invalid curve"_utf16);

        // 5. If the alg field of jwk is present and is not "Ed448" or "EdDSA", then throw a DataError.
        if (jwk.alg.has_value() && (jwk.alg.value() != "Ed448"sv && jwk.alg.value() != "EdDSA"sv))
            return WebIDL::DataError::create("Invalid algorithm"_utf16);

        // 6. If usages is non-empty and the use field of jwk is present and is not "sig", then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && jwk.use.value() != "sig"_utf16)
            return WebIDL::DataError::create("Invalid key usage"_utf16);

        // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
        //    or it does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(realm, jwk, usages));

        // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !jwk.ext.value() && extractable)
            return WebIDL::DataError::create("Invalid extractable"_utf16);

        // 9. If the d field is present:
        if (jwk.d.has_value()) {
            // 1. If jwk does not meet the requirements of the JWK private key format described in Section 2 of [RFC8037],
            //    then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create("Invalid key type"_utf16);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "Ed448"sv)
                return WebIDL::DataError::create("Invalid curve"_utf16);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create("Missing x field"_utf16);
            auto public_key = TRY(base64_url_bytes_decode(realm, jwk.x.value()));
            if (public_key.size() != 57)
                return WebIDL::DataError::create("Invalid key length"_utf16);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (!jwk.d.has_value())
                return WebIDL::DataError::create("Present d field"_utf16);
            auto private_key = TRY(base64_url_bytes_decode(realm, jwk.d.value()));
            if (private_key.size() != 57)
                return WebIDL::DataError::create("Invalid key length"_utf16);

            ::Crypto::Curves::Ed448 curve;
            auto derived_public_key = TRY_OR_THROW_OOM(realm.vm(), curve.generate_public_key(private_key));
            if (derived_public_key != public_key)
                return WebIDL::DataError::create("Invalid key pair"_utf16);

            // 2. Let key be a new CryptoKey object that represents the Ed448 private key identified by interpreting jwk according to Section 2 of [RFC8037].
            key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPrivateKey { move(private_key) } });

            // 3. Set the [[type]] internal slot of Key to "private".
            key->set_type(KeyType::Private);
        }
        // Otherwise:
        else {
            // 1. If jwk does not meet the requirements of the JWK public key format described in Section 2 of [RFC8037], then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create("Invalid key type"_utf16);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "Ed448"sv)
                return WebIDL::DataError::create("Invalid curve"_utf16);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create("Missing x field"_utf16);
            auto public_key = TRY(base64_url_bytes_decode(realm, jwk.x.value()));
            if (public_key.size() != 57)
                return WebIDL::DataError::create("Invalid key length"_utf16);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (jwk.d.has_value())
                return WebIDL::DataError::create("Present d field"_utf16);

            // 2. Let key be a new CryptoKey object that represents the Ed448 public key identified by interpreting jwk according to Section 2 of [RFC8037].
            key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(public_key) } });

            // 3. Set the [[type]] internal slot of Key to "public".
            key->set_type(KeyType::Public);
        }

        // 10. Let algorithm be a new instance of a KeyAlgorithm object.
        // 11. Set the name attribute of algorithm to "Ed448".
        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "Ed448"_utf16 });
    }

    // 2. If format is "raw":
    else if (format == KeyFormat::Raw) {
        // 1. If usages contains a value which is not "verify" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != KeyUsage::Verify) {
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
            }
        }

        // 2. Let data be keyData.
        auto data = move(key_data.get<ByteBuffer>());

        // 3. If the length in bits of data is not 448 then throw a DataError.
        // AD-HOC: The spec has a typo with the size of the key length
        //         See spec comment: https://github.com/w3c/webcrypto/pull/425#discussion_r3070135408
        if (data.size() * 8 != 456)
            return WebIDL::DataError::create("Invalid key length"_utf16);

        // 6. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents data.
        key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(data) } });

        // 7. Set the [[type]] internal slot of key to "public"
        key->set_type(KeyType::Public);

        // 4. Let algorithm be a new KeyAlgorithm object.
        // 5. Set the name attribute of algorithm to "Ed448".
        // 8. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "Ed448"_utf16 });
    }

    // 2. Otherwise:
    else {
        // throw a NotSupportedError.
        return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
    }

    return GC::Ref { *key };
}

// https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
WebIDL::ExceptionOr<ExportKeyResult> ED448::export_key(JS::Realm& realm, KeyFormat format, GC::Ref<CryptoKey> key)
{
    auto& vm = realm.vm();

    // 1. Let key be the CryptoKey to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    // 3. If format is "spki":
    if (format == KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != KeyType::Public)
            return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //      * Set the algorithm object identifier to the id-Ed448 OID defined in [RFC8410].
        //    * Set the subjectPublicKey field to keyData.
        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_subject_public_key_info(handle.get<OKPPublicKey>().bytes, ::Crypto::ASN1::ed448_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return ExportKeyResult { move(data) };
    }

    // 3. If format is "pkcs8":
    if (format == KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != KeyType::Private)
            return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

        // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
        //    * Set the version field to 0.
        //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        //      * Set the algorithm object identifier to the id-Ed448 OID defined in [RFC8410].
        //    * Set the privateKey field to the result of DER-encoding a CurvePrivateKey ASN.1 type,
        //      as defined in Section 7 of [RFC8410], that represents the Ed448 private key
        //      represented by the [[handle]] internal slot of key
        ::Crypto::ASN1::Encoder encoder;
        TRY_OR_THROW_OOM(vm, encoder.write(handle.get<OKPPrivateKey>().bytes.bytes()));

        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_private_key_info(encoder.finish(), ::Crypto::ASN1::ed448_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return ExportKeyResult { move(data) };
    }

    // 2. If format is "jwk":
    if (format == KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        JsonWebKey jwk;

        // 2. Set the kty attribute of jwk to "OKP".
        jwk.kty = "OKP"_utf16;

        // 3. Set the alg attribute of jwk to "Ed448".
        jwk.alg = "Ed448"_utf16;

        // 4. Set the crv attribute of jwk to "Ed448".
        jwk.crv = "Ed448"_utf16;

        // 5. Set the x attribute of jwk according to the definition in Section 2 of [RFC8037].
        if (key->type() == KeyType::Public) {
            jwk.x = Utf16String::from_utf8(TRY_OR_THROW_OOM(vm, encode_base64url(handle.get<OKPPublicKey>().bytes, AK::OmitPadding::Yes)));
        } else {
            // The "x" parameter of the "epk" field is set as follows:
            // Apply the appropriate ECDH function to the ephemeral private key (as scalar input)
            // and the standard base point (as u-coordinate input).
            // The base64url encoding of the output is the value for the "x" parameter of the "epk" field.
            ::Crypto::Curves::Ed448 curve;
            auto public_key = TRY_OR_THROW_OOM(vm, curve.generate_public_key(handle.get<OKPPrivateKey>().bytes));
            jwk.x = TRY_OR_THROW_OOM(vm, base64_url_bytes_encode(public_key));
        }

        // 6. If the [[type]] internal slot of key is "private"
        if (key->type() == KeyType::Private) {
            // 1. Set the d attribute of jwk according to the definition in Section 2 of [RFC8037].
            jwk.d = TRY_OR_THROW_OOM(vm, base64_url_bytes_encode(handle.get<OKPPrivateKey>().bytes));
        }

        // 7. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<Utf16String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages())
            jwk.key_ops->unchecked_append(Utf16String::from_utf8(key_usage_to_string(usage)));

        // 8. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 9. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        return ExportKeyResult { move(jwk) };
    }

    // 2. If format is "raw":
    if (format == KeyFormat::Raw) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != KeyType::Public)
            return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

        // 2. Let data be an octet string representing the Ed448 public key represented by the [[handle]] internal slot of key.
        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return ExportKeyResult { handle.get<OKPPublicKey>().bytes };
    }

    // 2. Otherwise:
    //    throw a NotSupportedError.
    return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
}

Optional<ReadonlySpan<KeyUsage>> ED448::public_key_usages() const
{
    static constexpr Array usages { KeyUsage::Verify };
    return usages.span();
}

// https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
WebIDL::ExceptionOr<ByteBuffer> ED448::sign(JS::Realm&, AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& message)
{
    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != KeyType::Private)
        return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

    // 2. Let context be the contents of the context member of normalizedAlgorithm
    //    or the empty octet string if the context member of normalizedAlgorithm is not present.
    auto const& algorithm = static_cast<Ed448Params const&>(params);
    auto context = algorithm.context.value_or(ByteBuffer());

    // 3. If context has a length greater than 255 bytes, then throw an OperationError.
    if (context.size() > 255)
        return WebIDL::OperationError::create("Context is too long"_utf16);

    // 4. Perform the Ed448 signing process, as specified in [RFC8032], Section 5.2.6,
    //    with message as M and context as C, using the Ed448 private key associated with key.
    ::Crypto::Curves::Ed448 curve;
    auto maybe_signature = curve.sign(key->handle().get<OKPPrivateKey>().bytes, message, context);
    if (maybe_signature.is_error()) {
        return WebIDL::OperationError::create("Failed to sign message"_utf16);
    }

    // 5. Return a new ArrayBuffer associated with the relevant global object of this [HTML],
    //    and containing the bytes of the signature resulting from performing the Ed448 signing process.
    return maybe_signature.release_value();
}

// https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
WebIDL::ExceptionOr<bool> ED448::verify(JS::Realm&, AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& signature, ByteBuffer const& message)
{
    // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
    if (key->type() != KeyType::Public)
        return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

    // 2. Let context be the contents of the context member of normalizedAlgorithm
    //    or the empty octet string if the context member of normalizedAlgorithm is not present.
    auto const& algorithm = static_cast<Ed448Params const&>(params);
    auto context = algorithm.context.value_or(ByteBuffer());

    // 3. If context has a length greater than 255 bytes, then throw an OperationError.
    if (context.size() > 255)
        return WebIDL::OperationError::create("Context is too long"_utf16);

    // 4. If the key data of key represents an invalid point or a small-order element
    //    on the Elliptic Curve of Ed448, return false.
    // NOTE: https://github.com/WICG/webcrypto-secure-curves/issues/27

    // 5. If the point R, encoded in the first half of signature, represents an invalid point
    //    or a small-order element on the Elliptic Curve of Ed448, return false.
    // NOTE: https://github.com/WICG/webcrypto-secure-curves/issues/27

    // 6. Perform the Ed448 verification steps, as specified in [RFC8032], Section 5.2.7, using
    //    the cofactorless (unbatched) equation, [S]B = R + [k]A', on the signature,
    //    with message as M and context as C, using the Ed448 public key associated with key.
    ::Crypto::Curves::Ed448 curve;
    auto maybe_verified = curve.verify(key->handle().get<OKPPublicKey>().bytes, signature, message, context);
    if (maybe_verified.is_error())
        return WebIDL::OperationError::create(Utf16String::from_utf8(maybe_verified.error().string_literal()));

    // 7. Let result be a boolean with the value true if the signature is valid
    //    and the value false otherwise.
    // 8. Return result.
    return maybe_verified.release_value();
}

// https://w3c.github.io/webcrypto/#x25519-operations-derive-bits
WebIDL::ExceptionOr<void> X25519::validate_derive_bits_parameters(AlgorithmParams const& params, Optional<u32> length_optional) const
{
    auto const& normalized_algorithm = static_cast<EcdhKeyDeriveParams const&>(params);

    // 1. Let publicKey be the public member of normalizedAlgorithm.
    auto& public_key = normalized_algorithm.public_key;

    // 2. If the [[type]] internal slot of publicKey is not "public", then throw an InvalidAccessError.
    if (public_key->type() != KeyType::Public)
        return WebIDL::InvalidAccessError::create("Public key is not a public key"_utf16);

    // 3. If the name attribute of the [[algorithm]] internal slot of publicKey is not equal to the name member of
    //    normalizedAlgorithm, then throw an InvalidAccessError.
    if (public_key->algorithm_name() != normalized_algorithm.name)
        return WebIDL::InvalidAccessError::create("Algorithm mismatch"_utf16);

    // 4. If length is not null and is greater than 256, then throw an OperationError.
    if (length_optional.has_value() && *length_optional > 256)
        return WebIDL::OperationError::create("Length is too large"_utf16);

    return {};
}

// https://w3c.github.io/webcrypto/#x25519-operations-derive-bits
WebIDL::ExceptionOr<ByteBuffer> X25519::derive_bits(JS::Realm& realm, AlgorithmParams const& params, GC::Ref<CryptoKey> key, Optional<u32> length_optional)
{
    auto const& normalized_algorithm = static_cast<EcdhKeyDeriveParams const&>(params);

    // NB: Steps 1 to 4 are performed by validate_derive_bits_parameters().
    TRY(validate_derive_bits_parameters(params, length_optional));
    auto& public_key = normalized_algorithm.public_key;

    // 5. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != KeyType::Private)
        return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

    // 6. If the name attribute of the [[algorithm]] internal slot of publicKey is not equal to
    //    the name property of the [[algorithm]] internal slot of key, then throw an InvalidAccessError.
    if (key->algorithm_name() != public_key->algorithm_name())
        return WebIDL::InvalidAccessError::create("Algorithm mismatch"_utf16);

    // 7. Let secret be the result of performing the X25519 function specified in [RFC7748] Section 5 with
    //    key as the X25519 private key k and
    //    the X25519 public key represented by the [[handle]] internal slot of publicKey as the X25519 public key u.
    auto const& private_key = key->handle().get<OKPPrivateKey>().bytes;
    auto const& public_key_data = public_key->handle().get<OKPPublicKey>().bytes;

    ::Crypto::Curves::X25519 curve;
    auto maybe_secret = curve.compute_coordinate(private_key, public_key_data);
    if (maybe_secret.is_error())
        return WebIDL::OperationError::create("Failed to compute secret"_utf16);

    auto secret = maybe_secret.release_value();

    // 8. If secret is the all-zero value, then throw a OperationError.
    //    This check must be performed in constant-time, as per [RFC7748] Section 6.1.
    // NOTE: The check may be performed by ORing all the bytes together and checking whether the result is zero,
    //       as this eliminates standard side-channels in software implementations.
    auto or_bytes = 0;
    for (auto byte : secret.bytes()) {
        or_bytes |= byte;
    }

    if (or_bytes == 0)
        return WebIDL::OperationError::create("Secret is the all-zero value"_utf16);

    // 9. If length is null: Return secret.
    if (!length_optional.has_value()) {
        auto result = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::copy(secret));
        return result;
    }

    // Otherwise: If the length of secret in bits is less than length: throw an OperationError.
    auto length = length_optional.value();
    if (secret.size() * 8 < length)
        return WebIDL::OperationError::create("Secret is too short"_utf16);

    // Otherwise: Return an octet string containing the first length bits of secret.
    auto slice = TRY_OR_THROW_OOM(realm.vm(), secret.slice(0, ceil_div(length, 8)));
    if (length % 8 != 0) {
        // Zero out the last bits
        slice[slice.size() - 1] &= 0xFF << (8 - (length % 8));
    }

    return slice;
}

// https://w3c.github.io/webcrypto/#x25519-operations-generate-key
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> X25519::generate_key(JS::Realm&, [[maybe_unused]] AlgorithmParams const& params, bool extractable, Vector<KeyUsage> const& key_usages)
{
    // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits) {
            return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
        }
    }

    // 2. Generate an X25519 key pair, with the private key being 32 random bytes,
    //    and the public key being X25519(a, 9), as defined in [RFC7748], section 6.1.
    ::Crypto::Curves::X25519 curve;
    auto maybe_private_key = curve.generate_private_key();
    if (maybe_private_key.is_error())
        return WebIDL::OperationError::create("Failed to generate private key"_utf16);

    auto private_key_data = maybe_private_key.release_value();

    auto maybe_public_key = curve.generate_public_key(private_key_data);
    if (maybe_public_key.is_error())
        return WebIDL::OperationError::create("Failed to generate public key"_utf16);

    auto public_key_data = maybe_public_key.release_value();

    // 5. Let publicKey be a new CryptoKey associated with the relevant global object of this [HTML],
    //    and representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(public_key_data) } });

    // 6. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(KeyType::Public);

    // 3. Let algorithm be a new KeyAlgorithm object.
    // 4. Set the name attribute of algorithm to "X25519".
    // 7. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(CryptoKey::KeyAlgorithmData { "X25519"_utf16 });

    // 8. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 9. Set the [[usages]] internal slot of publicKey to be the empty list.
    public_key->set_usages({});

    // 10. Let privateKey be a new CryptoKey associated with the relevant global object of this [HTML],
    //     and representing the private key of the generated key pair.
    auto private_key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPrivateKey { move(private_key_data) } });

    // 11. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(KeyType::Private);

    // 12. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(CryptoKey::KeyAlgorithmData { "X25519"_utf16 });

    // 13. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 14. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "deriveKey", "deriveBits" ].
    private_key->set_usages(usage_intersection(key_usages, { { KeyUsage::DeriveKey, KeyUsage::DeriveBits } }));

    // 15. Let result be a new CryptoKeyPair dictionary.
    // 16. Set the publicKey attribute of result to be publicKey.
    // 17. Set the privateKey attribute of result to be privateKey.
    // 18. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
    return Variant<GC::Ref<CryptoKey>, CryptoKeyPair> { CryptoKeyPair { public_key, private_key } };
}

// https://w3c.github.io/webcrypto/#x25519-operations-import-key
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> X25519::import_key(JS::Realm& realm, [[maybe_unused]] Web::Crypto::AlgorithmParams const& params, KeyFormat key_format, CryptoKey::ImportKeyData key_data, bool extractable, Vector<KeyUsage> const& usages)
{
    // NOTE: This is a parameter to the function
    // 1. Let keyData be the key data to be imported.

    auto& vm = realm.vm();
    GC::Ptr<CryptoKey> key = nullptr;

    // 2. If format is "spki":
    if (key_format == KeyFormat::Spki) {
        // 1. If usages is not empty then throw a SyntaxError.
        if (!usages.is_empty())
            return WebIDL::SyntaxError::create("Usages must be empty"_utf16);

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
        //    is not equal to the id-X25519 object identifier defined in [RFC8410], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::x25519_oid)
            return WebIDL::DataError::create("Invalid algorithm"_utf16);

        // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
        if (spki.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create("Invalid algorithm parameters"_utf16);

        // 6. Let publicKey be the X25519 public key identified by the subjectPublicKey field of spki.
        auto public_key = spki.raw_key;

        // 7. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents publicKey.
        key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(public_key) } });

        // 8. Set the [[type]] internal slot of key to "public"
        key->set_type(KeyType::Public);

        // 9. Let algorithm be a new KeyAlgorithm.
        // 10. Set the name attribute of algorithm to "X25519".
        // 11. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "X25519"_utf16 });
    }

    // 2. If format is "pkcs8":
    else if (key_format == KeyFormat::Pkcs8) {
        // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits) {
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
            }
        }

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo
        //    is not equal to the id-X25519 object identifier defined in [RFC8410], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::x25519_oid)
            return WebIDL::DataError::create("Invalid algorithm"_utf16);

        // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo is present, then throw a DataError.
        if (private_key_info.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create("Invalid algorithm parameters"_utf16);

        // 6. Let curvePrivateKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the privateKey field of privateKeyInfo,
        //    structure as the ASN.1 CurvePrivateKey structure specified in Section 7 of [RFC8410], and
        //    exactData set to true.
        // 7. If an error occurred while parsing, then throw a DataError.
        auto curve_private_key = TRY(parse_an_ASN1_structure<StringView>(realm, private_key_info.raw_key, true));
        auto curve_private_key_bytes = TRY_OR_THROW_OOM(vm, ByteBuffer::copy(curve_private_key.bytes()));

        // 8. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
        //    and that represents the X25519 private key identified by curvePrivateKey.
        key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPrivateKey { move(curve_private_key_bytes) } });

        // 9. Set the [[type]] internal slot of key to "private"
        key->set_type(KeyType::Private);

        // 10. Let algorithm be a new KeyAlgorithm.
        // 11. Set the name attribute of algorithm to "X25519".
        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "X25519"_utf16 });
    }

    // 2. If format is "jwk":
    else if (key_format == KeyFormat::Jwk) {
        // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
        //    Otherwise: Throw a DataError.
        if (!key_data.has<JsonWebKey>())
            return WebIDL::DataError::create("keyData is not a JsonWebKey dictionary"_utf16);
        auto& jwk = key_data.get<JsonWebKey>();

        // 2. If the d field is present and if usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        if (jwk.d.has_value() && !usages.is_empty()) {
            for (auto const& usage : usages) {
                if (usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits) {
                    return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
                }
            }
        }

        // 3. If the d field is not present and if usages is not empty then throw a SyntaxError.
        if (!jwk.d.has_value() && !usages.is_empty())
            return WebIDL::SyntaxError::create("Usages must be empty if d is missing"_utf16);

        // 4. If the kty field of jwk is not "OKP", then throw a DataError.
        if (jwk.kty != "OKP"sv)
            return WebIDL::DataError::create("Invalid key type"_utf16);

        // 5. If the crv field of jwk is not "X25519", then throw a DataError.
        if (jwk.crv != "X25519"sv)
            return WebIDL::DataError::create("Invalid curve"_utf16);

        // 6. If usages is non-empty and the use field of jwk is present and is not equal to "enc" then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && jwk.use.value() != "enc"sv)
            return WebIDL::DataError::create("Invalid use"_utf16);

        // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
        //    or it does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(realm, jwk, usages));

        // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !jwk.ext.value() && extractable)
            return WebIDL::DataError::create("Invalid extractable"_utf16);

        // 9. If the d field is present:
        if (jwk.d.has_value()) {
            // 1. If jwk does not meet the requirements of the JWK private key format described in Section 2 of [RFC8037], then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create("Invalid key type"_utf16);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "X25519"sv)
                return WebIDL::DataError::create("Invalid curve"_utf16);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create("Missing x field"_utf16);
            auto public_key = TRY(base64_url_bytes_decode(realm, jwk.x.value()));
            if (public_key.size() != 32)
                return WebIDL::DataError::create("Invalid key length"_utf16);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (!jwk.d.has_value())
                return WebIDL::DataError::create("Missing d field"_utf16);
            auto private_key = TRY(base64_url_bytes_decode(realm, jwk.d.value()));
            if (private_key.size() != 32)
                return WebIDL::DataError::create("Invalid key length"_utf16);

            ::Crypto::Curves::X25519 curve;
            auto derived_public_key = TRY_OR_THROW_OOM(realm.vm(), curve.generate_public_key(private_key));
            if (derived_public_key != public_key)
                return WebIDL::DataError::create("Invalid key pair"_utf16);

            // 2. Let key be a new CryptoKey object that represents the X25519 private key identified by interpreting jwk according to Section 2 of [RFC8037].
            key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPrivateKey { move(private_key) } });

            // 3. Set the [[type]] internal slot of Key to "private".
            key->set_type(KeyType::Private);
        }
        // 9. Otherwise:
        else {
            // 1. If jwk does not meet the requirements of the JWK public key format described in Section 2 of [RFC8037], then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create("Invalid key type"_utf16);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "X25519"sv)
                return WebIDL::DataError::create("Invalid curve"_utf16);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create("Missing x field"_utf16);
            auto public_key = TRY(base64_url_bytes_decode(realm, jwk.x.value()));
            if (public_key.size() != 32)
                return WebIDL::DataError::create("Invalid key length"_utf16);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (jwk.d.has_value())
                return WebIDL::DataError::create("Present d field"_utf16);

            key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(public_key) } });

            // 3. Set the [[type]] internal slot of Key to "public".
            key->set_type(KeyType::Public);
        }

        // 10. Let algorithm be a new instance of a KeyAlgorithm object.
        // 11. Set the name attribute of algorithm to "X25519".
        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "X25519"_utf16 });
    }

    // 2. If format is "raw":
    // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
    // For all existing asymmetric algorithms in WebCrypto, "raw-public" acts as an alias of "raw".
    else if (key_format == KeyFormat::Raw || key_format == KeyFormat::RawPublic) {
        // 1. If usages is not empty then throw a SyntaxError.
        if (!usages.is_empty())
            return WebIDL::SyntaxError::create("Usages must be empty"_utf16);

        // 2. If the length in bits of keyData is not 256 then throw a DataError.
        auto& data = key_data.get<ByteBuffer>();
        if (data.size() * 8 != 256)
            return WebIDL::DataError::create("Invalid key length"_utf16);

        // 5. Let key be a new CryptoKey representing the key data provided in keyData.
        key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(data) } });

        // 6. Set the [[type]] internal slot of key to "public"
        key->set_type(KeyType::Public);

        // 3. Let algorithm be a new KeyAlgorithm object.
        // 4. Set the name attribute of algorithm to "X25519".
        // 7. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "X25519"_utf16 });
    }

    // 2. Otherwise: throw a NotSupportedError.
    else {
        return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
    }

    // 3. Return key.
    return GC::Ref { *key };
}

// https://w3c.github.io/webcrypto/#x25519-operations-export-key
WebIDL::ExceptionOr<ExportKeyResult> X25519::export_key(JS::Realm& realm, KeyFormat format, GC::Ref<CryptoKey> key)
{
    auto& vm = realm.vm();

    // NOTE: This is a parameter to the function
    // 1. Let key be the CryptoKey to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    Optional<ExportKeyResult> result;

    // 3. If format is "spki":
    if (format == KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != KeyType::Public)
            return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //    Set the algorithm object identifier to the id-X25519 OID defined in [RFC8410].
        //    Set the subjectPublicKey field to keyData.
        auto public_key = handle.get<OKPPublicKey>().bytes;
        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_subject_public_key_info(public_key, ::Crypto::ASN1::x25519_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        result = ExportKeyResult { data };
    }

    // 3. If format is "pkcs8":
    else if (format == KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != KeyType::Private)
            return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

        // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
        //    Set the version field to 0.
        //    Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        //    Set the algorithm object identifier to the id-X25519 OID defined in [RFC8410].
        //    Set the privateKey field to the result of DER-encoding a CurvePrivateKey ASN.1 type, as defined in Section 7 of [RFC8410],
        //    that represents the X25519 private key represented by the [[handle]] internal slot of key
        auto private_key = handle.get<OKPPrivateKey>().bytes;

        ::Crypto::ASN1::Encoder encoder;
        TRY_OR_THROW_OOM(vm, encoder.write(private_key));
        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_private_key_info(encoder.finish(), ::Crypto::ASN1::x25519_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        result = ExportKeyResult { data };
    }

    // 3. If format is "jwt":
    else if (format == KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionar1y.
        JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to "OKP".
        jwk.kty = "OKP"_utf16;

        // 3. Set the crv attribute of jwk to "X25519".
        jwk.crv = "X25519"_utf16;

        // 4. Set the x attribute of jwk according to the definition in Section 2 of [RFC8037].
        if (key->type() == KeyType::Public) {
            auto public_key = handle.get<OKPPublicKey>().bytes;
            jwk.x = Utf16String::from_utf8(TRY_OR_THROW_OOM(vm, encode_base64url(public_key, AK::OmitPadding::Yes)));
        } else {
            // The "x" parameter of the "epk" field is set as follows:
            // Apply the appropriate ECDH function to the ephemeral private key (as scalar input)
            // and the standard base point (as u-coordinate input).
            // The base64url encoding of the output is the value for the "x" parameter of the "epk" field.
            ::Crypto::Curves::X25519 curve;
            auto public_key = TRY_OR_THROW_OOM(vm, curve.generate_public_key(handle.get<OKPPrivateKey>().bytes));
            jwk.x = TRY_OR_THROW_OOM(vm, base64_url_bytes_encode(public_key));
        }

        // 5. If the [[type]] internal slot of key is "private"
        if (key->type() == KeyType::Private) {
            // 1. Set the d attribute of jwk according to the definition in Section 2 of [RFC8037].
            auto private_key = handle.get<OKPPrivateKey>().bytes;
            jwk.d = TRY_OR_THROW_OOM(vm, base64_url_bytes_encode(private_key));
        }

        // 6. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<Utf16String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages())
            jwk.key_ops->unchecked_append(Utf16String::from_utf8(key_usage_to_string(usage)));

        // 7. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 8. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        result = ExportKeyResult { move(jwk) };
    }

    // 3. If format is "raw":
    // https://wicg.github.io/webcrypto-modern-algos/#subtlecrypto-interface-keyformat
    // For all existing asymmetric algorithms in WebCrypto, "raw-public" acts as an alias of "raw".
    else if (format == KeyFormat::Raw || format == KeyFormat::RawPublic) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != KeyType::Public)
            return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

        // 2. Let data be an octet string representing the X25519 public key represented by the [[handle]] internal slot of key.
        auto public_key = handle.get<OKPPublicKey>().bytes;

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        result = ExportKeyResult { public_key };
    }

    // 3. Otherwise:
    else {
        return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
    }

    // 4. Return result.
    return result.release_value();
}

Optional<ReadonlySpan<KeyUsage>> X25519::public_key_usages() const
{
    return ReadonlySpan<KeyUsage> {};
}

// https://wicg.github.io/webcrypto-secure-curves/#x448-operations
WebIDL::ExceptionOr<ByteBuffer> X448::derive_bits(JS::Realm& realm,
    AlgorithmParams const& params,
    GC::Ref<CryptoKey> key,
    Optional<u32> length_optional)
{
    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != KeyType::Private)
        return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

    // 2. Let publicKey be the public member of normalizedAlgorithm.
    auto& public_key = static_cast<EcdhKeyDeriveParams const&>(params).public_key;

    // 3. If the [[type]] internal slot of publicKey is not "public", then throw an InvalidAccessError.
    if (public_key->type() != KeyType::Public)
        return WebIDL::InvalidAccessError::create("Public key is not a public key"_utf16);

    // 4. If the name attribute of the [[algorithm]] internal slot of publicKey is not equal to
    //    the name property of the [[algorithm]] internal slot of key, then throw an InvalidAccessError.
    if (key->algorithm_name() != public_key->algorithm_name())
        return WebIDL::InvalidAccessError::create("Algorithm mismatch"_utf16);

    // 5. Let secret be the result of performing the X448 function specified in [RFC7748] Section 5
    //    with key as the X448 private key k and the X448 public key represented by the [[handle]]
    //    internal slot of publicKey as the X448 public key u.
    auto const& private_key = key->handle().get<OKPPrivateKey>().bytes;
    auto const& public_key_data = public_key->handle().get<OKPPublicKey>().bytes;

    ::Crypto::Curves::X448 curve;
    auto maybe_secret = curve.compute_coordinate(private_key, public_key_data);
    if (maybe_secret.is_error())
        return WebIDL::OperationError::create("Failed to compute secret"_utf16);

    auto secret = maybe_secret.release_value();

    // 6. If secret is the all-zero value, then throw a OperationError. This check must be performed in constant-time, as per [RFC7748] Section 6.2.
    // NOTE: The check may be performed by ORing all the bytes together and checking whether the result is zero,
    //       as this eliminates standard side-channels in software implementations.
    auto or_bytes = 0;
    for (auto byte : secret.bytes()) {
        or_bytes |= byte;
    }

    if (or_bytes == 0)
        return WebIDL::OperationError::create("Secret is the all-zero value"_utf16);

    // 7. If length is null: Return secret
    if (!length_optional.has_value()) {
        auto result = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::copy(secret));
        return result;
    }

    // Otherwise: Return an octet string containing the first length bits of secret.
    auto length = length_optional.value();
    if (secret.size() * 8 < length)
        return WebIDL::OperationError::create("Secret is too short"_utf16);

    auto slice = TRY_OR_THROW_OOM(realm.vm(), secret.slice(0, ceil_div(length, 8)));
    if (length % 8 != 0) {
        // Zero out the last bits
        slice[slice.size() - 1] &= 0xFF << (8 - (length % 8));
    }

    return slice;
}

// https://wicg.github.io/webcrypto-secure-curves/#x448-operations
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> X448::generate_key(JS::Realm&,
    AlgorithmParams const&,
    bool extractable,
    Vector<KeyUsage> const& usages)
{
    // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
    for (auto const& usage : usages) {
        if (usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits) {
            return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
        }
    }

    // 2. Generate an X448 key pair, with the private key being 56 random bytes, and the public key being X448(a, 5), as defined in [RFC7748], section 6.2.
    ::Crypto::Curves::X448 curve;
    auto maybe_private_key = curve.generate_private_key();
    if (maybe_private_key.is_error())
        return WebIDL::OperationError::create("Failed to generate private key"_utf16);
    auto private_key_data = maybe_private_key.release_value();
    auto maybe_public_key = curve.generate_public_key(private_key_data);
    if (maybe_public_key.is_error())
        return WebIDL::OperationError::create("Failed to generate public key"_utf16);
    auto public_key_data = maybe_public_key.release_value();

    // 5. Let publicKey be a new CryptoKey associated with the relevant global object of this [HTML], and representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(public_key_data) } });

    // 6. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(KeyType::Public);

    // 3. Let algorithm be a new KeyAlgorithm object.
    // 4. Set the name attribute of algorithm to "X448".
    // 7. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(CryptoKey::KeyAlgorithmData { "X448"_utf16 });

    // 8. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 9. Set the [[usages]] internal slot of publicKey to be the empty list.
    public_key->set_usages({});

    // 10. Let privateKey be a new CryptoKey associated with the relevant global object of this [HTML], and representing the private key of the generated key pair.
    auto private_key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPrivateKey { move(private_key_data) } });

    // 11. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(KeyType::Private);

    // 12. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(CryptoKey::KeyAlgorithmData { "X448"_utf16 });

    // 13. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 14. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "deriveKey", "deriveBits" ].
    private_key->set_usages(usage_intersection(usages, { { KeyUsage::DeriveKey, KeyUsage::DeriveBits } }));

    // 15. Let result be a new CryptoKeyPair dictionary.
    // 16. Set the publicKey attribute of result to be publicKey.
    // 17. Set the privateKey attribute of result to be privateKey.
    auto result = CryptoKeyPair { public_key, private_key };

    // 18. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
    return Variant<GC::Ref<CryptoKey>, CryptoKeyPair> { result };
}

// https://wicg.github.io/webcrypto-secure-curves/#x448-operations
WebIDL::ExceptionOr<ExportKeyResult> X448::export_key(JS::Realm& realm, KeyFormat format, GC::Ref<CryptoKey> key)
{
    // 1. Let key be the CryptoKey to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    // 3. If format is "spki":
    if (format == KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != KeyType::Public)
            return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //      * Set the algorithm object identifier to the id-X448 OID defined in [RFC8410].
        //    * Set the subjectPublicKey field to keyData.
        auto x448_oid = ::Crypto::ASN1::x448_oid;
        auto data = TRY_OR_THROW_OOM(realm.vm(), ::Crypto::PK::wrap_in_subject_public_key_info(handle.get<OKPPublicKey>().bytes, x448_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return ExportKeyResult { data };
    }

    // 3. If format is "pkcs8":
    if (format == KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != KeyType::Private)
            return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

        // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
        //    * Set the version field to 0.
        //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        //      * Set the algorithm object identifier to the id-X448 OID defined in [RFC8410].
        //    * Set the privateKey field to the result of DER-encoding a CurvePrivateKey ASN.1 type, as defined in Section 7 of [RFC8410], that represents the X448 private key represented by the [[handle]] internal slot of key
        ::Crypto::ASN1::Encoder encoder;
        TRY_OR_THROW_OOM(realm.vm(), encoder.write(handle.get<OKPPrivateKey>().bytes.bytes()));

        auto x448_oid = ::Crypto::ASN1::x448_oid;
        auto data = TRY_OR_THROW_OOM(realm.vm(), ::Crypto::PK::wrap_in_private_key_info(encoder.finish(), x448_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return ExportKeyResult { data };
    }

    // 3. If format is "jwk":
    if (format == KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to "OKP".
        jwk.kty = "OKP"_utf16;

        // 3. Set the crv attribute of jwk to "X448".
        jwk.crv = "X448"_utf16;

        // 4. Set the x attribute of jwk according to the definition in Section 2 of [RFC8037].
        if (key->type() == KeyType::Public) {
            jwk.x = Utf16String::from_utf8(TRY_OR_THROW_OOM(realm.vm(), encode_base64url(handle.get<OKPPublicKey>().bytes, AK::OmitPadding::Yes)));
        } else {
            ::Crypto::Curves::X448 curve;
            auto public_key = TRY_OR_THROW_OOM(realm.vm(), curve.generate_public_key(handle.get<OKPPrivateKey>().bytes));
            jwk.x = Utf16String::from_utf8(TRY_OR_THROW_OOM(realm.vm(), encode_base64url(public_key, AK::OmitPadding::Yes)));
        }

        // 5. If the [[type]] internal slot of key is "private"
        if (key->type() == KeyType::Private) {
            // 1. Set the d attribute of jwk according to the definition in Section 2 of [RFC8037].
            jwk.d = Utf16String::from_utf8(TRY_OR_THROW_OOM(realm.vm(), encode_base64url(handle.get<OKPPrivateKey>().bytes, AK::OmitPadding::Yes)));
        }

        // 6. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<Utf16String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages())
            jwk.key_ops->unchecked_append(Utf16String::from_utf8(key_usage_to_string(usage)));

        // 7. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 8. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        return ExportKeyResult { move(jwk) };
    }

    // 3. If format is "raw":
    if (format == KeyFormat::Raw) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != KeyType::Public)
            return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

        // 2. Let data be an octet string representing the X448 public key represented by the [[handle]] internal slot of key.
        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return ExportKeyResult { handle.get<OKPPublicKey>().bytes };
    }

    // 3. Otherwise:
    //    throw a NotSupportedError.
    return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
}

Optional<ReadonlySpan<KeyUsage>> X448::public_key_usages() const
{
    return ReadonlySpan<KeyUsage> {};
}

// https://wicg.github.io/webcrypto-secure-curves/#x448-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> X448::import_key(JS::Realm& realm,
    AlgorithmParams const&,
    KeyFormat format,
    CryptoKey::ImportKeyData key_data,
    bool extractable,
    Vector<KeyUsage> const& usages)
{
    // 1. Let keyData be the key data to be imported.

    // 2. If format is "spki":
    if (format == KeyFormat::Spki) {
        // 1. If usages is not empty then throw a SyntaxError.
        if (!usages.is_empty())
            return WebIDL::SyntaxError::create("Usages must be empty"_utf16);

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki is not equal to the id-X448 object identifier defined in [RFC8410], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::x448_oid)
            return WebIDL::DataError::create("Invalid algorithm"_utf16);

        // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
        if (spki.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create("Invalid algorithm parameters"_utf16);

        // 6. Let publicKey be the X448 public key identified by the subjectPublicKey field of spki.
        auto const& public_key = spki.raw_key;

        // 7. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents publicKey.
        auto key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(public_key) } });

        // 8. Set the [[type]] internal slot of key to "public"
        key->set_type(KeyType::Public);

        // 9. Let algorithm be a new KeyAlgorithm.
        // 10. Set the name attribute of algorithm to "X448".
        // 11. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "X448"_utf16 });

        return key;
    }

    // 3. If format is "pkcs8":
    if (format == KeyFormat::Pkcs8) {
        // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits) {
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
            }
        }

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(realm, key_data.get<ByteBuffer>()));
        auto private_key = private_key_info.raw_key;

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo is not equal to the id-X448 object identifier defined in [RFC8410], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::x448_oid)
            return WebIDL::DataError::create("Invalid algorithm"_utf16);

        // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo is present, then throw a DataError.
        if (private_key_info.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create("Invalid algorithm parameters"_utf16);

        // 6. Let curvePrivateKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 CurvePrivateKey
        //    structure specified in Section 7 of [RFC8410], and exactData set to true.
        // 7. If an error occurred while parsing, then throw a DataError.
        auto curve_private_key = TRY(parse_an_ASN1_structure<StringView>(realm, private_key_info.raw_key, true));
        auto curve_private_key_bytes = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::copy(curve_private_key.bytes()));

        // 8. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents the X448 private key identified by curvePrivateKey.
        auto key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPrivateKey { move(curve_private_key_bytes) } });

        // 9. Set the [[type]] internal slot of key to "private"
        key->set_type(KeyType::Private);

        // 10. Let algorithm be a new KeyAlgorithm.
        // 11. Set the name attribute of algorithm to "X448".
        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "X448"_utf16 });

        return key;
    }

    // 3. If format is "jwk":
    if (format == KeyFormat::Jwk) {
        // 1. If keyData is a JsonWebKey dictionary:
        //    Let jwk equal keyData.
        //    Otherwise:
        //    Throw a DataError.
        if (!key_data.has<JsonWebKey>())
            return WebIDL::DataError::create("Data is not a JsonWebKey dictionary"_utf16);
        auto& jwk = key_data.get<JsonWebKey>();

        // 2. If the d field is present and if usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        if (jwk.d.has_value()) {
            for (auto const& usage : usages) {
                if (usage != KeyUsage::DeriveKey && usage != KeyUsage::DeriveBits) {
                    return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
                }
            }
        }

        // 3. If the d field is not present and if usages is not empty then throw a SyntaxError.
        if (!jwk.d.has_value() && !usages.is_empty())
            return WebIDL::SyntaxError::create("Usages must be empty"_utf16);

        // 4. If the kty field of jwk is not "OKP", then throw a DataError.
        if (jwk.kty != "OKP"sv)
            return WebIDL::DataError::create("Invalid key type"_utf16);

        // 5. If the crv field of jwk is not "X448", then throw a DataError.
        if (jwk.crv != "X448"sv)
            return WebIDL::DataError::create("Invalid curve"_utf16);

        // 6. If usages is non-empty and the use field of jwk is present and is not equal to "enc" then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && jwk.use.value() != "enc"sv)
            return WebIDL::DataError::create("Invalid use"_utf16);

        // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
        //    or it does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(realm, jwk, usages));

        // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !jwk.ext.value() && extractable)
            return WebIDL::DataError::create("Invalid extractable"_utf16);

        GC::Ptr<CryptoKey> key = nullptr;

        // 9. If the d field is present:
        if (jwk.d.has_value()) {
            // 1. If jwk does not meet the requirements of the JWK private key format described in Section 2 of [RFC8037], then throw a DataError.

            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create("Invalid key type"_utf16);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "X448"sv)
                return WebIDL::DataError::create("Invalid curve"_utf16);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create("Missing x field"_utf16);
            auto public_key = TRY(base64_url_bytes_decode(realm, jwk.x.value()));
            if (public_key.size() != 56)
                return WebIDL::DataError::create("Invalid key length"_utf16);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (!jwk.d.has_value())
                return WebIDL::DataError::create("Missing d field"_utf16);
            auto private_key = TRY(base64_url_bytes_decode(realm, jwk.d.value()));
            if (private_key.size() != 56)
                return WebIDL::DataError::create("Invalid key length"_utf16);

            ::Crypto::Curves::X448 curve;
            auto derived_public_key = TRY_OR_THROW_OOM(realm.vm(), curve.generate_public_key(private_key));
            if (derived_public_key != public_key)
                return WebIDL::DataError::create("Invalid key pair"_utf16);

            // 2. Let key be a new CryptoKey object that represents the X448 private key identified by interpreting jwk according to Section 2 of [RFC8037].
            key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPrivateKey { move(private_key) } });

            // 3. Set the [[type]] internal slot of Key to "private".
            key->set_type(KeyType::Private);
        }
        // Otherwise:
        else {
            // 1. If jwk does not meet the requirements of the JWK public key format described in Section 2 of [RFC8037], then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create("Invalid key type"_utf16);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "X448"sv)
                return WebIDL::DataError::create("Invalid curve"_utf16);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create("Missing x field"_utf16);
            auto public_key = TRY(base64_url_bytes_decode(realm, jwk.x.value()));
            if (public_key.size() != 56)
                return WebIDL::DataError::create("Invalid key length"_utf16);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (jwk.d.has_value())
                return WebIDL::DataError::create("Present d field"_utf16);

            // 2. Let key be a new CryptoKey object that represents the X448 public key identified by interpreting jwk according to Section 2 of [RFC8037].
            key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(public_key) } });

            // 3. Set the [[type]] internal slot of Key to "public".
            key->set_type(KeyType::Public);
        }

        // 10. Let algorithm be a new instance of a KeyAlgorithm object.
        // 11. Set the name attribute of algorithm to "X448".
        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "X448"_utf16 });

        return GC::Ref { *key };
    }

    // 3. If format is "raw":
    if (format == KeyFormat::Raw) {
        // 1. If usages is not empty then throw a SyntaxError.
        if (!usages.is_empty())
            return WebIDL::SyntaxError::create("Usages must be empty"_utf16);

        // 2.Let data be keyData.
        auto data = move(key_data.get<ByteBuffer>());

        // 3. If the length in bits of data is not 448 then throw a DataError.
        if (data.size() * 8 != 448)
            return WebIDL::DataError::create("Invalid key length"_utf16);

        // 6. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents data.
        auto key = CryptoKey::create(CryptoKey::InternalKeyData { OKPPublicKey { move(data) } });

        // 7. Set the [[type]] internal slot of key to "public"
        key->set_type(KeyType::Public);

        // 4. Let algorithm be a new KeyAlgorithm object.
        // 5. Set the name attribute of algorithm to "X448".
        // 8. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { "X448"_utf16 });

        return key;
    }

    (void)extractable;

    // 3. Otherwise:
    //    throw a NotSupportedError.
    return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
}

extern "C" {
void web_crypto_hmac_sign(::Crypto::Hash::HashKind, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);
bool web_crypto_timing_safe_compare(ReadonlyBytes, ReadonlyBytes);
}

static_assert(::Crypto::Hash::SHA1::block_size() == 64);
static_assert(::Crypto::Hash::SHA256::block_size() == 64);
static_assert(::Crypto::Hash::SHA384::block_size() == 128);
static_assert(::Crypto::Hash::SHA512::block_size() == 128);

void web_crypto_hmac_sign(::Crypto::Hash::HashKind hash_kind, ReadonlyBytes key, ReadonlyBytes message, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Authentication::HMAC hmac(hash_kind, key);
    auto mac = hmac.process(message);
    receive(state, mac);
}

bool web_crypto_timing_safe_compare(ReadonlyBytes left, ReadonlyBytes right)
{
    VERIFY(left.size() == right.size());
    return timing_safe_compare(left.data(), right.data(), left.size());
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-sign
WebIDL::ExceptionOr<void> MLDSA::validate_sign_parameters(AlgorithmParams const& params) const
{
    auto const& normalized_algorithm = static_cast<ContextParams const&>(params);

    // 1. If the context member of normalizedAlgorithm is present and has a length greater than 255 bytes, then throw
    //    an OperationError.
    if (normalized_algorithm.context.has_value() && normalized_algorithm.context->size() > 255)
        return WebIDL::OperationError::create("Context is too long"_utf16);

    return {};
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-sign
WebIDL::ExceptionOr<ByteBuffer> MLDSA::sign(JS::Realm&, AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& message)
{
    auto const& normalized_algorithm = static_cast<ContextParams const&>(params);

    // NB: Step 1 is performed by validate_sign_parameters().
    TRY(validate_sign_parameters(params));

    // 2. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != KeyType::Private)
        return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

    // 3. Let context be the context member of normalizedAlgorithm or the empty octet string if the
    //    context member of normalizedAlgorithm is not present.
    auto context = normalized_algorithm.context.value_or(ByteBuffer());

    // 4. Let result be the result of performing the ML-DSA.Sign signing algorithm, as specified in
    //    Section 5.2 of [FIPS-204], with the parameter set indicated by the name member of normalizedAlgorithm,
    //    using the ML-DSA private key associated with key as sk, message as M and context as ctx.
    VERIFY(key->handle().has<::Crypto::PK::MLDSAPrivateKey>());
    auto lattice = [&] {
        if (params.name == "ML-DSA-44"sv)
            return ::Crypto::PK::MLDSA(::Crypto::PK::MLDSASize::MLDSA44, key->handle().get<::Crypto::PK::MLDSAPrivateKey>(), context);
        if (params.name == "ML-DSA-65"sv)
            return ::Crypto::PK::MLDSA(::Crypto::PK::MLDSASize::MLDSA65, key->handle().get<::Crypto::PK::MLDSAPrivateKey>(), context);
        if (params.name == "ML-DSA-87"sv)
            return ::Crypto::PK::MLDSA(::Crypto::PK::MLDSASize::MLDSA87, key->handle().get<::Crypto::PK::MLDSAPrivateKey>(), context);
        VERIFY_NOT_REACHED();
    }();
    auto const maybe_result = lattice.sign(message);

    // 5. If the ML-DSA.Sign algorithm returned an error, return an OperationError.
    if (maybe_result.is_error())
        return WebIDL::OperationError::create("Failed to sign message"_utf16);

    // 6. Return result.
    return maybe_result.value();
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-verify
WebIDL::ExceptionOr<void> MLDSA::validate_verify_parameters(AlgorithmParams const& params) const
{
    auto const& normalized_algorithm = static_cast<ContextParams const&>(params);

    // 1. If the context member of normalizedAlgorithm is present and has a length greater than 255 bytes, then throw
    //    an OperationError.
    if (normalized_algorithm.context.has_value() && normalized_algorithm.context->size() > 255)
        return WebIDL::OperationError::create("Context is too long"_utf16);

    return {};
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-verify
WebIDL::ExceptionOr<bool> MLDSA::verify(JS::Realm&, AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& signature, ByteBuffer const& message)
{
    auto const& normalized_algorithm = static_cast<ContextParams const&>(params);

    // NB: Step 1 is performed by validate_verify_parameters().
    TRY(validate_verify_parameters(params));

    // 2. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
    if (key->type() != KeyType::Public)
        return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

    // 3. Let context be the context member of normalizedAlgorithm or the empty octet string if the
    //    context member of normalizedAlgorithm is not present.
    auto context = normalized_algorithm.context.value_or(ByteBuffer());

    // 4. Let result be the result of performing the ML-DSA.Verify verification algorithm, as specified in
    //    Section 5.3 of [FIPS-204], with the parameter set indicated by the name member of normalizedAlgorithm,
    //    using the ML-DSA public key associated with key as pk, message as M, signature as σ and context as ctx.
    VERIFY(key->handle().has<::Crypto::PK::MLDSAPublicKey>());
    auto lattice = [&] {
        if (params.name == "ML-DSA-44"sv)
            return ::Crypto::PK::MLDSA(::Crypto::PK::MLDSASize::MLDSA44, key->handle().get<::Crypto::PK::MLDSAPublicKey>(), context);
        if (params.name == "ML-DSA-65"sv)
            return ::Crypto::PK::MLDSA(::Crypto::PK::MLDSASize::MLDSA65, key->handle().get<::Crypto::PK::MLDSAPublicKey>(), context);
        if (params.name == "ML-DSA-87"sv)
            return ::Crypto::PK::MLDSA(::Crypto::PK::MLDSASize::MLDSA87, key->handle().get<::Crypto::PK::MLDSAPublicKey>(), context);
        VERIFY_NOT_REACHED();
    }();
    auto const maybe_result = lattice.verify(message, signature);

    // 5. If the ML-DSA.Verify algorithm returned an error, return an OperationError.
    if (maybe_result.is_error())
        return WebIDL::OperationError::create("Failed to verify message"_utf16);

    // 6. Return result.
    return maybe_result.value();
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-generate-key
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> MLDSA::generate_key(JS::Realm&, AlgorithmParams const& params, bool extractable, Vector<KeyUsage> const& usages)
{
    // 1. If usages contains an entry which is not one of "sign" or "verify", then throw a SyntaxError.
    for (auto const& usage : usages) {
        if (usage != KeyUsage::Sign && usage != KeyUsage::Verify) {
            return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
        }
    }

    // 2. Generate an ML-DSA key pair, as described in Section 5.1 of [FIPS-204], with the parameter set indicated by the name member of normalizedAlgorithm.
    auto const maybe_key_pair = [&] {
        if (params.name == "ML-DSA-44"sv)
            return ::Crypto::PK::MLDSA::generate_key_pair(::Crypto::PK::MLDSA44);
        if (params.name == "ML-DSA-65"sv)
            return ::Crypto::PK::MLDSA::generate_key_pair(::Crypto::PK::MLDSA65);
        if (params.name == "ML-DSA-87"sv)
            return ::Crypto::PK::MLDSA::generate_key_pair(::Crypto::PK::MLDSA87);
        VERIFY_NOT_REACHED();
    }();

    // 3. If the key generation step fails, then throw an OperationError.
    if (maybe_key_pair.is_error())
        return WebIDL::OperationError::create("Key generation failed"_utf16);
    auto const& key_pair = maybe_key_pair.value();

    // 6. Let publicKey be a new CryptoKey representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(key_pair.public_key);

    // 7. Set the [[type]] internal slot of publicKey to "public".
    public_key->set_type(KeyType::Public);

    // 4. Let algorithm be a new KeyAlgorithm object.
    // 5. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
    // 8. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });

    // 9. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 10. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
    public_key->set_usages(usage_intersection(usages, { { KeyUsage::Verify } }));

    // 11. Let privateKey be a new CryptoKey representing the private key of the generated key pair.
    auto private_key = CryptoKey::create(key_pair.private_key);

    // 12. Set the [[type]] internal slot of privateKey to "private".
    private_key->set_type(KeyType::Private);

    // 13. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });

    // 14. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 15. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
    private_key->set_usages(usage_intersection(usages, { { KeyUsage::Sign } }));

    // 16. Let result be a new CryptoKeyPair dictionary.
    // 17. Set the publicKey attribute of result to be publicKey.
    // 18. Set the privateKey attribute of result to be privateKey.
    auto const result = CryptoKeyPair { public_key, private_key };

    // 19. Return result.
    return Variant<GC::Ref<CryptoKey>, CryptoKeyPair> { result };
}

static ErrorOr<::Crypto::PK::MLDSA::KeyPairType> generate_mldsa_key_pair_from_seed(Utf16String const& name, ByteBuffer const& seed)
{
    if (name == "ML-DSA-44"sv)
        return ::Crypto::PK::MLDSA::generate_key_pair(::Crypto::PK::MLDSA44, seed);
    if (name == "ML-DSA-65"sv)
        return ::Crypto::PK::MLDSA::generate_key_pair(::Crypto::PK::MLDSA65, seed);
    if (name == "ML-DSA-87"sv)
        return ::Crypto::PK::MLDSA::generate_key_pair(::Crypto::PK::MLDSA87, seed);
    VERIFY_NOT_REACHED();
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-import-key
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> MLDSA::import_key(JS::Realm& realm, AlgorithmParams const& params, KeyFormat key_format, CryptoKey::ImportKeyData key_data, bool extractable, Vector<KeyUsage> const& usages)
{
    GC::Ptr<CryptoKey> key = nullptr;

    // 1. Let keyData be the key data to be imported.
    // 2. -> If format is "spki":
    if (key_format == KeyFormat::Spki) {
        // 1. If usages contains an entry which is not "verify" then throw a SyntaxError.
        for (auto const usage : usages) {
            if (usage != KeyUsage::Verify)
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
        }

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto const spki = TRY(parse_a_subject_public_key_info(realm, key_data.get<ByteBuffer>()));

        Array<u32, 9> expected_oid;
        // 4. If the name member of normalizedAlgorithm is "ML-DSA-44":
        if (params.name == "ML-DSA-44"sv) {
            // Let expectedOid be id-ml-dsa-44 (2.16.840.1.101.3.4.3.17).
            expected_oid = ::Crypto::ASN1::ml_dsa_44_oid;
        }
        // If the name member of normalizedAlgorithm is "ML-DSA-65":
        else if (params.name == "ML-DSA-65"sv) {
            // Let expectedOid be id-ml-dsa-65 (2.16.840.1.101.3.4.3.18).
            expected_oid = ::Crypto::ASN1::ml_dsa_65_oid;
        }
        // If the name member of normalizedAlgorithm is "ML-DSA-87":
        else if (params.name == "ML-DSA-87"sv) {
            // Let expectedOid be id-ml-dsa-87 (2.16.840.1.101.3.4.3.19).
            expected_oid = ::Crypto::ASN1::ml_dsa_87_oid;
        }
        // Otherwise:
        else {
            // throw a NotSupportedError.
            return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
        }

        // 5. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki is not equal
        //    to expectedOid, then throw a DataError.
        if (spki.algorithm.identifier != expected_oid)
            return WebIDL::DataError::create("Invalid algorithm"_utf16);

        // 6. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
        if (spki.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create("Invalid algorithm parameters"_utf16);

        // 7. Let publicKey be the ML-DSA public key identified by the subjectPublicKey field of spki.
        auto const public_key = spki.raw_key;

        // 8. Let key be a new CryptoKey that represents publicKey.
        key = CryptoKey::create(::Crypto::PK::MLDSAPublicKey(public_key));

        // 9. Set the [[type]] internal slot of key to "public"
        key->set_type(KeyType::Public);

        // 10. Let algorithm be a new KeyAlgorithm.
        // 11. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });
    }
    // -> If format is "pkcs8":
    else if (key_format == KeyFormat::Pkcs8) {
        // 1. If usages contains an entry which is not "sign" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != KeyUsage::Sign) {
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
            }
        }

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        VERIFY(key_data.has<ByteBuffer>());
        auto private_key_info = TRY(parse_a_private_key_info(realm, key_data.get<ByteBuffer>()));

        // 4. => If the name member of normalizedAlgorithm is "ML-DSA-44":
        //       Let expectedOid be id-ml-dsa-44 (2.16.840.1.101.3.4.3.17).
        //       Let asn1Structure be the ASN.1 ML-DSA-44-PrivateKey structure.
        //    => If the name member of normalizedAlgorithm is "ML-DSA-65":
        //       Let expectedOid be id-ml-dsa-65 (2.16.840.1.101.3.4.3.18).
        //       Let asn1Structure be the ASN.1 ML-DSA-65-PrivateKey structure.
        //    => If the name member of normalizedAlgorithm is "ML-DSA-87":
        //       Let expectedOid be id-ml-dsa-87 (2.16.840.1.101.3.4.3.19).
        //       Let asn1Structure be the ASN.1 ML-DSA-67-PrivateKey structure.
        //    => Otherwise:
        //       throw a NotSupportedError.
        Array<u32, 9> expected_oid {};
        if (params.name == "ML-DSA-44"sv) {
            expected_oid = ::Crypto::ASN1::ml_dsa_44_oid;
        } else if (params.name == "ML-DSA-65"sv) {
            expected_oid = ::Crypto::ASN1::ml_dsa_65_oid;
        } else if (params.name == "ML-DSA-87"sv) {
            expected_oid = ::Crypto::ASN1::ml_dsa_87_oid;
        } else {
            return WebIDL::NotSupportedError::create("Invalid algorithm"_utf16);
        }

        // 5. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of
        //    privateKeyInfo is not equal to expectedOid, then throw a DataError.
        if (private_key_info.algorithm.identifier != expected_oid)
            return WebIDL::DataError::create("Invalid algorithm"_utf16);

        // 6. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of
        //    privateKeyInfo is present, then throw a DataError.
        if (private_key_info.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create("Invalid algorithm parameters"_utf16);

        // 7. Let mlDsaPrivateKey be the result of performing the parse an ASN.1 structure algorithm, with
        //    data as the privateKey field of privateKeyInfo, structure as asn1Structure, and exactData set to
        //    true.
        // NOTE: We already did this in parse_a_private_key_info
        // 8. If an error occurred while parsing, then throw a DataError.
        auto& ml_dsa_private_key = private_key_info.mldsa;

        // 9. Let key be a new CryptoKey that represents the ML-DSA private key identified by
        //    mlDsaPrivateKey.
        key = CryptoKey::create(ml_dsa_private_key);

        // 10. Set the [[type]] internal slot of key to "private"
        key->set_type(KeyType::Private);

        // 11. Let algorithm be a new KeyAlgorithm.
        // 12. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
        // 13. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });
    }
    //    -> If format is "raw-public":
    else if (key_format == KeyFormat::RawPublic) {
        // 1. If usages contains an entry which is not "verify" then throw a SyntaxError.
        for (auto const usage : usages) {
            if (usage != KeyUsage::Verify)
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
        }

        // 4. Let key be a new CryptoKey representing the key data provided in keyData.
        ASSERT(key_data.has<ByteBuffer>());
        key = CryptoKey::create(::Crypto::PK::MLDSAPublicKey { key_data.get<ByteBuffer>() });

        // 5. Set the [[type]] internal slot of key to "public"
        key->set_type(KeyType::Public);

        // 2. Let algorithm be a new KeyAlgorithm object.
        // 3. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
        // 6. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });
    }
    //  -> If format is "raw-seed":
    else if (key_format == KeyFormat::RawSeed) {
        // 1. If usages contains an entry which is not "sign" then throw a SyntaxError.
        for (auto const usage : usages) {
            if (usage != KeyUsage::Sign)
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
        }

        // 2. Let data be keyData.
        VERIFY(key_data.has<ByteBuffer>());
        auto const data = key_data.get<ByteBuffer>();

        // 3. If the length in bits of data is not 256 then throw a DataError.
        if (data.size() * 8 != 256)
            return WebIDL::DataError::create(Utf16String::formatted("Seed must be 256 bits long"));

        // 4. Let privateKey be the result of performing the ML-DSA.KeyGen_internal function described in Section 6.1
        //    of [FIPS-204] with the parameter set indicated by the name member of normalizedAlgorithm, using data as ξ.
        auto maybe_key_pair = generate_mldsa_key_pair_from_seed(params.name, data);
        if (maybe_key_pair.is_error())
            return WebIDL::OperationError::create(Utf16String::formatted("Key generation failed: {}", maybe_key_pair.release_error()));
        auto const private_key = maybe_key_pair.release_value().private_key;

        // 5. Let key be a new CryptoKey that represents the ML-DSA private key identified by privateKey.
        key = CryptoKey::create(private_key);

        // 6. Set the [[type]] internal slot of key to "private"
        key->set_type(KeyType::Private);

        // 7. Let algorithm be a new KeyAlgorithm.
        // 8. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
        // Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });
    }
    //  -> If format is "jwk":
    else if (key_format == KeyFormat::Jwk) {
        // 1.  => If keyData is a JsonWebKey dictionary:
        //        Let jwk equal keyData.
        //     => Otherwise:
        //        Throw a DataError.
        auto* const jwk = key_data.get_pointer<JsonWebKey>();

        if (!jwk)
            return WebIDL::DataError::create("Data is not a JsonWebKey dictionary"_utf16);

        // 2. If the priv field is present and usages contains an entry which is not "sign", or, if the priv field is
        //    not present and usages contains an entry which is not "verify" then throw a SyntaxError.
        if ((jwk->priv.has_value() && usages.contains([](auto usage) { return usage != KeyUsage::Sign; }))
            || (!jwk->priv.has_value() && usages.contains([](auto usage) { return usage != KeyUsage::Verify; }))) {
            return WebIDL::SyntaxError::create("Invalid usage"_utf16);
        }

        // 3. If the kty field of jwk is not "AKP", then throw a DataError.
        if (jwk->kty != "AKP"_utf16)
            return WebIDL::DataError::create("Invalid key type"_utf16);

        // 4. If the alg field of jwk is not equal to the name member of normalizedAlgorithm, then throw a DataError.
        if (jwk->alg != params.name)
            return WebIDL::DataError::create("Invalid algorithm"_utf16);

        // 5. If usages is non-empty and the use field of jwk is present and is not equal to "sig", then throw a DataError.
        if (!usages.is_empty() && jwk->use.has_value() && jwk->use != "sig"_utf16)
            return WebIDL::DataError::create("Invalid usage type"_utf16);

        // 6. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web
        //    Key [JWK], or it does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(realm, *jwk, usages));

        // 7. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk->ext.has_value() && jwk->ext == false && extractable)
            return WebIDL::DataError::create("Invalid ext field"_utf16);

        // 8. => If the priv field of jwk is present:
        if (jwk->priv.has_value()) {
            // 1. If the priv attribute of jwk does not contain a valid base64url encoded seed
            //    representing an ML-DSA private key, then throw a DataError.
            auto const seed = TRY(base64_url_bytes_decode(realm, jwk->priv.value()));
            if (seed.size() * 8 != 256)
                return WebIDL::DataError::create("Seed must be 256 bits long"_utf16);

            // 2. Let key be a new CryptoKey object that represents the ML-DSA private key
            //    identified by interpreting the priv attribute of jwk as a base64url encoded seed.
            auto maybe_key_pair = generate_mldsa_key_pair_from_seed(params.name, seed);
            if (maybe_key_pair.is_error())
                return WebIDL::DataError::create(Utf16String::formatted("Invalid seed: {}", maybe_key_pair.release_error()));
            auto const [public_key, private_key] = maybe_key_pair.release_value();
            key = CryptoKey::create(private_key);

            // 3. Set the [[type]] internal slot of key to "private".
            key->set_type(KeyType::Private);

            // 4. If the pub attribute of jwk does not contain the base64url encoded public key
            //    representing the ML-DSA public key corresponding to key, then throw a DataError.
            if (!jwk->pub.has_value())
                return WebIDL::DataError::create("JsonWebKey does not contain public key"_utf16);

            if (auto pub_decoded = base64_url_bytes_decode(realm, jwk->pub.value());
                pub_decoded.is_error() || pub_decoded.value() != public_key.public_key())
                return WebIDL::DataError::create("JsonWebKey public key does not match"_utf16);
        }
        //    => Otherwise:
        else {
            // 1. If the pub attribute of jwk does not contain a valid base64url encoded public key
            //    representing an ML-DSA public key, then throw a DataError.
            if (!jwk->pub.has_value())
                return WebIDL::DataError::create("JsonWebKey does not contain public key"_utf16);

            auto public_key = TRY(base64_url_bytes_decode(realm, jwk->pub.value()));

            // 2. Let key be a new CryptoKey object that represents the ML-DSA public key
            //    identified by interpreting the pub attribute of jwk as a base64url encoded public key.
            key = CryptoKey::create(::Crypto::PK::MLDSAPublicKey { public_key });

            // 3. Set the [[type]] internal slot of key to "public".
            key->set_type(KeyType::Public);
        }

        // 9. Let algorithm be a new instance of a KeyAlgorithm object.
        // 10. Set the name attribute of algorithm to the name member of normalizedAlgorithm.
        // 11. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });
    }
    //    -> Otherwise:
    else {
        // throw a NotSupportedError.
        return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
    }

    // 3. Return key.
    return GC::Ref { *key };
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-dsa-operations-export-key
WebIDL::ExceptionOr<ExportKeyResult> MLDSA::export_key(JS::Realm& realm, KeyFormat format, GC::Ref<CryptoKey> key)
{
    Optional<ExportKeyResult> result;
    auto& vm = realm.vm();

    // 1. Let key be the CryptoKey to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    // 3. -> If format is "spki":
    if (format == KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != KeyType::Public)
            return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

        // 2. Let keyAlgorithm be the [[algorithm]] internal slot of key.

        // 3. Let data be an instance of the SubjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //      * -> If the name member of keyAlgorithm is "ML-DSA-44":
        //           Set the algorithm object identifier to the id-ml-dsa-44 (2.16.840.1.101.3.4.3.17) OID.
        //        -> If the name member of keyAlgorithm is "ML-DSA-65":
        //           Set the algorithm object identifier to the id-ml-dsa-65 (2.16.840.1.101.3.4.3.18) OID.
        //        -> If the name member of keyAlgorithm is "ML-DSA-87":
        //           Set the algorithm object identifier to the id-ml-dsa-87 (2.16.840.1.101.3.4.3.19) OID.
        //        -> Otherwise:
        //           throw a NotSupportedError.
        //    * Set the subjectPublicKey field to keyData.
        Array<u32, 9> algorithm_oid {};
        if (key->algorithm_name() == "ML-DSA-44"sv) {
            algorithm_oid = ::Crypto::ASN1::ml_dsa_44_oid;
        } else if (key->algorithm_name() == "ML-DSA-65"sv) {
            algorithm_oid = ::Crypto::ASN1::ml_dsa_65_oid;
        } else if (key->algorithm_name() == "ML-DSA-87"sv) {
            algorithm_oid = ::Crypto::ASN1::ml_dsa_87_oid;
        } else {
            return WebIDL::NotSupportedError::create("Invalid algorithm"_utf16);
        }

        ::Crypto::ASN1::Encoder encoder;
        VERIFY(handle.has<::Crypto::PK::MLDSAPublicKey>());
        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_subject_public_key_info(handle.get<::Crypto::PK::MLDSAPublicKey>().public_key(), algorithm_oid));

        // 4. Let result be the result of DER-encoding data.
        result = ExportKeyResult { data };
    }
    //   -> If format is "pkcs8":
    else if (format == KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != KeyType::Private)
            return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

        // 2. Let keyAlgorithm be the [[algorithm]] internal slot of key.

        // 3. Let data be an instance of the PrivateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
        //    * Set the version field to 0.
        //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        //      * => If the name member of keyAlgorithm is "ML-DSA-44":
        //           Set the algorithm object identifier to the id-ml-dsa-44 (2.16.840.1.101.3.4.3.17) OID.
        //      * => If the name member of keyAlgorithm is "ML-DSA-65":
        //           Set the algorithm object identifier to the id-ml-dsa-65 (2.16.840.1.101.3.4.3.18) OID.
        //      * => If the name member of keyAlgorithm is "ML-DSA-87":
        //           Set the algorithm object identifier to the id-ml-dsa-87 (2.16.840.1.101.3.4.3.19) OID.
        //      * => Otherwise:
        //           throw a NotSupportedError.
        //    * Set the privateKey field as follows:
        //      * => If the name member of keyAlgorithm is "ML-DSA-44":
        //           Set the privateKey field to the result of DER-encoding a ML-DSA-44-PrivateKey
        //           ASN.1 type that represents the ML-DSA private key seed represented by the
        //           [[handle]] internal slot of key using the seed-only format (using a context-
        //           specific [0] primitive tag with an implicit encoding of OCTET STRING).
        //      * => If the name member of keyAlgorithm is "ML-DSA-65":
        //           Set the privateKey field to the result of DER-encoding a ML-DSA-65-PrivateKey
        //           ASN.1 type that represents the ML-DSA private key seed represented by the
        //           [[handle]] internal slot of key using the seed-only format (using a context-
        //           specific [0] primitive tag with an implicit encoding of OCTET STRING).
        //      * => If the name member of keyAlgorithm is "ML-DSA-87":
        //           Set the privateKey field to the result of DER-encoding a ML-DSA-87-PrivateKey
        //           ASN.1 type that represents the ML-DSA private key seed represented by the
        //           [[handle]] internal slot of key using the seed-only format (using a context-
        //           specific [0] primitive tag with an implicit encoding of OCTET STRING).
        //      * => Otherwise:
        //           throw a NotSupportedError.
        Array<u32, 9> algorithm_oid {};
        if (key->algorithm_name() == "ML-DSA-44"sv) {
            algorithm_oid = ::Crypto::ASN1::ml_dsa_44_oid;
        } else if (key->algorithm_name() == "ML-DSA-65"sv) {
            algorithm_oid = ::Crypto::ASN1::ml_dsa_65_oid;
        } else if (key->algorithm_name() == "ML-DSA-87"sv) {
            algorithm_oid = ::Crypto::ASN1::ml_dsa_87_oid;
        } else {
            return WebIDL::NotSupportedError::create("Invalid algorithm"_utf16);
        }

        ::Crypto::ASN1::Encoder encoder;
        VERIFY(handle.has<::Crypto::PK::MLDSAPrivateKey>());
        auto const data = TRY_OR_THROW_OOM(realm.vm(), ::Crypto::PK::wrap_in_private_key_info(handle.get<::Crypto::PK::MLDSAPrivateKey>(), algorithm_oid));

        // 4. Let result be the result of DER-encoding data.
        result = ExportKeyResult { data };
    }
    //   -> If format is "raw-public":
    else if (format == KeyFormat::RawPublic) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != KeyType::Public)
            return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

        // 2. Let data be a byte sequence containing the ML-DSA public key represented by the [[handle]] internal slot of key.
        VERIFY(handle.has<::Crypto::PK::MLDSAPublicKey>());
        auto const data = handle.get<::Crypto::PK::MLDSAPublicKey>().public_key();

        // 3. Let result be data.
        result = ExportKeyResult { data };
    }
    //   -> If format is "raw-seed":
    else if (format == KeyFormat::RawSeed) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != KeyType::Private)
            return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

        // 2. Let data be a byte sequence containing the ξ seed variable of the key represented by the [[handle]] internal slot of key.
        VERIFY(handle.has<::Crypto::PK::MLDSAPrivateKey>());
        auto const data = handle.get<::Crypto::PK::MLDSAPrivateKey>().seed();

        // 3. Let result be data.
        result = ExportKeyResult { data };
    }
    //   -> If format is "jwk":
    else if (format == KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        auto jwk = JsonWebKey {};

        // 2. Let keyAlgorithm be the [[algorithm]] internal slot of key.

        // 3. Set the kty attribute of jwk to "AKP".
        jwk.kty = "AKP"_utf16;

        // 4. Set the alg attribute of jwk to the name member of keyAlgorithm.
        jwk.alg = key->algorithm_name();

        // 5. Set the pub attribute of jwk to the base64url encoded public key corresponding to the [[handle]] internal slot of key.
        jwk.pub = TRY_OR_THROW_OOM(vm, base64_url_bytes_encode(handle.visit([](::Crypto::PK::MLDSAPublicKey const& public_key) -> ReadonlyBytes { return public_key.public_key(); }, [](::Crypto::PK::MLDSAPrivateKey const& private_key) -> ReadonlyBytes { return private_key.public_key(); }, [](auto) -> ReadonlyBytes { VERIFY_NOT_REACHED(); })));

        // 6. -> If the [[type]] internal slot of key is "private":
        //       Set the priv attribute of jwk to the base64url encoded seed represented by the [[handle]] internal slot of key.
        if (key->type() == KeyType::Private) {
            VERIFY(handle.has<::Crypto::PK::MLDSAPrivateKey>());
            jwk.priv = TRY_OR_THROW_OOM(vm, base64_url_bytes_encode(handle.get<::Crypto::PK::MLDSAPrivateKey>().seed()));
        }

        // 7. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<Utf16String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages()) {
            jwk.key_ops->unchecked_append(Utf16String::from_utf8(key_usage_to_string(usage)));
        }

        // 8. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 9. Let result be jwk.
        result = ExportKeyResult { move(jwk) };
    }
    //   -> Otherwise:
    else {
        // throw a NotSupportedError.
        return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
    }

    // 4. Return result.
    return result.release_value();
}

Optional<ReadonlySpan<KeyUsage>> MLDSA::public_key_usages() const
{
    static constexpr Array usages { KeyUsage::Verify };
    return usages.span();
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-kem-operations-generate-key
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, CryptoKeyPair>> MLKEM::generate_key(JS::Realm&, AlgorithmParams const& params, bool extractable, Vector<KeyUsage> const& usages)
{
    // 1. If usages contains an entry which is not one of "encapsulateKey", "encapsulateBits", "decapsulateKey" or
    //    "decapsulateBits", then throw a SyntaxError.
    for (auto const usage : usages) {
        if (usage != KeyUsage::EncapsulateKey
            && usage != KeyUsage::EncapsulateBits
            && usage != KeyUsage::DecapsulateKey
            && usage != KeyUsage::DecapsulateBits) {
            return WebIDL::SyntaxError::create("Invalid key usage"_utf16);
        }
    }

    // 2. Generate an ML-KEM key pair, as described in Section 7.1 of [FIPS-203], with the parameter set indicated
    //    by the name member of normalizedAlgorithm.
    auto maybe_key_pair = [&] {
        if (params.name == "ML-KEM-512"sv)
            return ::Crypto::PK::MLKEM::generate_key_pair(::Crypto::PK::MLKEMSize::MLKEM512);
        if (params.name == "ML-KEM-768"sv)
            return ::Crypto::PK::MLKEM::generate_key_pair(::Crypto::PK::MLKEMSize::MLKEM768);
        if (params.name == "ML-KEM-1024"sv)
            return ::Crypto::PK::MLKEM::generate_key_pair(::Crypto::PK::MLKEMSize::MLKEM1024);
        VERIFY_NOT_REACHED();
    }();

    // 3. If the key generation step fails, then throw an OperationError.
    if (maybe_key_pair.is_error())
        return WebIDL::OperationError::create(Utf16String::formatted("Key generation failed: {}", maybe_key_pair.release_error()));
    auto const key_pair = maybe_key_pair.release_value();

    // 6. Let publicKey be a new CryptoKey representing the encapsulation key of the generated key pair.
    auto public_key = CryptoKey::create(key_pair.public_key);

    // 7. Set the [[type]] internal slot of publicKey to "public".
    public_key->set_type(KeyType::Public);

    // 4. Let algorithm be a new KeyAlgorithm object.
    // 5. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
    // 8. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });

    // 9. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 10. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "encapsulateKey",
    //     "encapsulateBits" ].
    public_key->set_usages(usage_intersection(usages, { { KeyUsage::EncapsulateKey, KeyUsage::EncapsulateBits } }));

    // 11. Let privateKey be a new CryptoKey representing the decapsulation key of the generated key pair.
    auto private_key = CryptoKey::create(key_pair.private_key);

    // 12. Set the [[type]] internal slot of privateKey to "private".
    private_key->set_type(KeyType::Private);

    // 13. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });

    // 14. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 15. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "decapsulateKey",
    //     "decapsulateBits" ].
    private_key->set_usages(usage_intersection(usages, { { KeyUsage::DecapsulateKey, KeyUsage::DecapsulateBits } }));

    // 16. Let result be a new CryptoKeyPair dictionary.
    // 17. Set the publicKey attribute of result to be publicKey.
    // 18. Set the privateKey attribute of result to be privateKey.
    auto result = CryptoKeyPair { public_key, private_key };

    // 19. Return result.
    return Variant<GC::Ref<CryptoKey>, CryptoKeyPair> { result };
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-kem-operations-import-key
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> MLKEM::import_key(JS::Realm& realm, AlgorithmParams const& params, KeyFormat format, CryptoKey::ImportKeyData key_data, bool, Vector<KeyUsage> const& usages)
{
    GC::Ptr<CryptoKey> key = nullptr;

    // 1. Let keyData be the key data to be imported.
    // 2. -> If format is "spki":
    if (format == KeyFormat::Spki) {
        // 1. If usages contains an entry which is not "encapsulateKey" or "encapsulateBits" then throw
        //    a SyntaxError.
        for (auto const usage : usages) {
            if (usage != KeyUsage::EncapsulateKey && usage != KeyUsage::EncapsulateBits)
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
        }

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto const spki = TRY(parse_a_subject_public_key_info(realm, key_data.get<ByteBuffer>()));

        Array<u32, 9> expected_oid;
        // 4. If the name member of normalizedAlgorithm is "ML-KEM-512":
        if (params.name == "ML-KEM-512"sv) {
            // Let expectedOid be id-alg-ml-kem-512 (2.16.840.1.101.3.4.4.1).
            expected_oid = ::Crypto::ASN1::ml_kem_512_oid;
        }
        // If the name member of normalizedAlgorithm is "ML-KEM-768":
        else if (params.name == "ML-KEM-768"sv) {
            // Let expectedOid be id-alg-ml-kem-768 (2.16.840.1.101.3.4.4.2).
            expected_oid = ::Crypto::ASN1::ml_kem_768_oid;
        }
        // If the name member of normalizedAlgorithm is "ML-KEM-1024":
        else if (params.name == "ML-KEM-1024"sv) {
            // Let expectedOid be id-alg-ml-kem-1024 (2.16.840.1.101.3.4.4.3).
            expected_oid = ::Crypto::ASN1::ml_kem_1024_oid;
        }
        // Otherwise:
        else {
            // throw a NotSupportedError.
            return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
        }

        // 5. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki is not equal
        //    to expectedOid, then throw a DataError.
        if (spki.algorithm.identifier != expected_oid)
            return WebIDL::DataError::create("Invalid algorithm"_utf16);

        // 6. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
        if (spki.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create("Invalid algorithm parameters"_utf16);

        // 7. Let publicKey be the ML-KEM public key identified by the subjectPublicKey field of spki.
        auto const public_key = spki.raw_key;

        // 8. Let key be a new CryptoKey that represents publicKey.
        key = CryptoKey::create(::Crypto::PK::MLKEMPublicKey(public_key));

        // 9. Set the [[type]] internal slot of key to "public"
        key->set_type(KeyType::Public);

        // 10. Let algorithm be a new KeyAlgorithm.
        // 11. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });
    }
    //    -> If format is "pkcs8":
    else if (format == KeyFormat::Pkcs8) {
        // 1. If usages contains an entry which is not "decapsulateKey" or "decapsulateBits" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != KeyUsage::DecapsulateKey && usage != KeyUsage::DecapsulateBits) {
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
            }
        }

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        VERIFY(key_data.has<ByteBuffer>());
        auto private_key_info = TRY(parse_a_private_key_info(realm, key_data.get<ByteBuffer>()));

        // 4. => If the name member of normalizedAlgorithm is "ML-KEM-512":
        //       Let expectedOid be id-alg-ml-kem-512 (2.16.840.1.101.3.4.4.1).
        //       Let asn1Structure be the ASN.1 ML-KEM-512-PrivateKey structure.
        //    => If the name member of normalizedAlgorithm is "ML-KEM-768":
        //       Let expectedOid be id-alg-ml-kem-768 (2.16.840.1.101.3.4.4.2).
        //       Let asn1Structure be the ASN.1 ML-KEM-768-PrivateKey structure.
        //    => If the name member of normalizedAlgorithm is "ML-KEM-1024":
        //       Let expectedOid be id-alg-ml-kem-1024 (2.16.840.1.101.3.4.4.3).
        //       Let asn1Structure be the ASN.1 ML-KEM-1024-PrivateKey structure.
        //    => Otherwise:
        //       throw a NotSupportedError.
        Array<u32, 9> expected_oid {};
        if (params.name == "ML-KEM-512"sv) {
            expected_oid = ::Crypto::ASN1::ml_kem_512_oid;
        } else if (params.name == "ML-KEM-768"sv) {
            expected_oid = ::Crypto::ASN1::ml_kem_768_oid;
        } else if (params.name == "ML-KEM-1024"sv) {
            expected_oid = ::Crypto::ASN1::ml_kem_1024_oid;
        } else {
            return WebIDL::NotSupportedError::create("Invalid algorithm"_utf16);
        }

        // 5. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of
        //    privateKeyInfo is not equal to expectedOid, then throw a DataError.
        if (private_key_info.algorithm.identifier != expected_oid)
            return WebIDL::DataError::create("Invalid algorithm"_utf16);

        // 6. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of
        //    privateKeyInfo is present, then throw a DataError.
        if (private_key_info.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create("Invalid algorithm parameters"_utf16);

        // 7. Let mlKemPrivateKey be the result of performing the parse an ASN.1 structure algorithm, with
        //    data as the privateKey field of privateKeyInfo, structure as asn1Structure, and exactData set to
        //    true.
        // NOTE: We already did this in parse_a_private_key_info
        // 8. If an error occurred while parsing, then throw a DataError.
        auto& ml_kem_private_key = private_key_info.mlkem;

        // 9. Let key be a new CryptoKey that represents the ML-KEM private key identified by
        //    mlKemPrivateKey.
        key = CryptoKey::create(ml_kem_private_key);

        // 10. Set the [[type]] internal slot of key to "private"
        key->set_type(KeyType::Private);

        // 11. Let algorithm be a new KeyAlgorithm.
        // 12. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
        // 13. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });
    }
    //    -> If format is "raw-public":
    else if (format == KeyFormat::RawPublic) {
        // 1. If usages contains an entry which is not "encapsulateKey" or "encapsulateBits" then throw a SyntaxError.
        for (auto const usage : usages) {
            if (usage != KeyUsage::EncapsulateKey && usage != KeyUsage::EncapsulateBits)
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
        }

        // 2. Let data be keyData.
        auto const& data = key_data;

        // 3. Let key be a new CryptoKey that represents the ML-KEM public key data in data.
        ASSERT(data.has<ByteBuffer>());
        key = CryptoKey::create(::Crypto::PK::MLKEMPublicKey { data.get<ByteBuffer>() });

        // 4. Set the [[type]] internal slot of key to "public"
        key->set_type(KeyType::Public);

        // 5. Let algorithm be a new KeyAlgorithm object.
        // 6. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
        // 6. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });
    }
    //    -> If format is "raw-seed":
    else if (format == KeyFormat::RawSeed) {
        // 1. If usages contains an entry which is not "decapsulateKey" or "decapsulateBits" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != KeyUsage::DecapsulateKey && usage != KeyUsage::DecapsulateBits) {
                return WebIDL::SyntaxError::create(Utf16String::formatted("Invalid key usage '{}'", key_usage_to_string(usage)));
            }
        }

        // 2. Let data be keyData.
        VERIFY(key_data.has<ByteBuffer>());
        auto const data = move(key_data.get<ByteBuffer>());

        // 3. If the length in bits of data is not 512 then throw a DataError.
        if (data.size() * 8 != 512)
            return WebIDL::DataError::create("Invalid key format"_utf16);

        // 4. Let privateKey be the result of performing the ML-KEM.KeyGen_internal function described in
        //    Section 6.1 of [FIPS-203] with the parameter set indicated by the name member of
        //    normalizedAlgorithm, using the first 256 bits of data as d and the last 256 bits of data as z.
        auto maybe_key_pair = [&] {
            if (params.name == "ML-KEM-512"sv)
                return ::Crypto::PK::MLKEM::generate_key_pair(::Crypto::PK::MLKEMSize::MLKEM512, data);
            if (params.name == "ML-KEM-768"sv)
                return ::Crypto::PK::MLKEM::generate_key_pair(::Crypto::PK::MLKEMSize::MLKEM768, data);
            if (params.name == "ML-KEM-1024"sv)
                return ::Crypto::PK::MLKEM::generate_key_pair(::Crypto::PK::MLKEMSize::MLKEM1024, data);
            VERIFY_NOT_REACHED();
        }();
        if (maybe_key_pair.is_error())
            return WebIDL::OperationError::create(Utf16String::formatted("Key generation failed: {}", maybe_key_pair.release_error()));
        auto const key_pair = maybe_key_pair.release_value();

        // 5. Let key be a new CryptoKey that represents the ML-KEM private key identified by privateKey.
        key = CryptoKey::create(key_pair.private_key);

        // 6. Set the [[type]] internal slot of key to "private"
        key->set_type(KeyType::Private);

        // 7. Let algorithm be a new KeyAlgorithm.
        // 8. Set the name attribute of algorithm to the name attribute of normalizedAlgorithm.
        // 9. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(CryptoKey::KeyAlgorithmData { params.name });
    }
    // FIXME: -> If format is "jwk":
    //    -> Otherwise:
    else {
        // throw a NotSupportedError.
        return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
    }

    // 3. Return key.
    return GC::Ref { *key };
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-kem-operations-export-key
WebIDL::ExceptionOr<ExportKeyResult> MLKEM::export_key(JS::Realm& realm, KeyFormat format, GC::Ref<CryptoKey> key)
{
    Optional<ExportKeyResult> result;
    auto& vm = realm.vm();

    // 1. Let key be the CryptoKey to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be
    //    accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    // 3. -> If format is "spki":
    if (format == KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != KeyType::Public)
            return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

        // 2. Let keyAlgorithm be the [[algorithm]] internal slot of key.

        // 3. Let data be an instance of the SubjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //      * -> If the name member of keyAlgorithm is "ML-KEM-512":
        //           Set the algorithm object identifier to the id-alg-ml-kem-512 (2.16.840.1.101.3.4.4.1) OID.
        //        -> If the name member of keyAlgorithm is "ML-KEM-768":
        //           Set the algorithm object identifier to the id-alg-ml-kem-768 (2.16.840.1.101.3.4.4.2) OID.
        //        -> If the name member of keyAlgorithm is "ML-KEM-1024":
        //           Set the algorithm object identifier to the id-alg-ml-kem-1024 (2.16.840.1.101.3.4.4.3) OID.
        //        -> Otherwise:
        //           throw a NotSupportedError.
        //    * Set the subjectPublicKey field to keyData.
        Array<u32, 9> algorithm_oid {};
        if (key->algorithm_name() == "ML-KEM-512"sv) {
            algorithm_oid = ::Crypto::ASN1::ml_kem_512_oid;
        } else if (key->algorithm_name() == "ML-KEM-768"sv) {
            algorithm_oid = ::Crypto::ASN1::ml_kem_768_oid;
        } else if (key->algorithm_name() == "ML-KEM-1024"sv) {
            algorithm_oid = ::Crypto::ASN1::ml_kem_1024_oid;
        } else {
            return WebIDL::NotSupportedError::create("Invalid algorithm"_utf16);
        }

        ::Crypto::ASN1::Encoder encoder;
        VERIFY(handle.has<::Crypto::PK::MLKEMPublicKey>());
        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_subject_public_key_info(handle.get<::Crypto::PK::MLKEMPublicKey>().public_key(), algorithm_oid));

        // 4. Let result be the result of DER-encoding data.
        result = ExportKeyResult { data };
    }
    //   -> If format is "pkcs8":
    else if (format == KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != KeyType::Private)
            return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

        // 2. Let keyAlgorithm be the [[algorithm]] internal slot of key.

        // 3. Let data be an instance of the PrivateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
        //    * Set the version field to 0.
        //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        //      * => If the name member of keyAlgorithm is "ML-KEM-512":
        //           Set the algorithm object identifier to the id-alg-ml-kem-512 (2.16.840.1.101.3.4.4.1) OID.
        //      * => If the name member of keyAlgorithm is "ML-KEM-768":
        //           Set the algorithm object identifier to the id-alg-ml-kem-768 (2.16.840.1.101.3.4.4.2) OID.
        //      * => If the name member of keyAlgorithm is "ML-KEM-1024":
        //           Set the algorithm object identifier to the id-alg-ml-kem-1024 (2.16.840.1.101.3.4.4.3) OID.
        //      * => Otherwise:
        //           throw a NotSupportedError.
        //    * Set the privateKey field as follows:
        //      * => If the name member of keyAlgorithm is "ML-KEM-512":
        //           Set the privateKey field to the result of DER-encoding a ML-KEM-512-PrivateKey
        //           ASN.1 type that represents the ML-KEM private key seed represented by the
        //           [[handle]] internal slot of key using the seed-only format (using a context-
        //           specific [0] primitive tag with an implicit encoding of OCTET STRING).
        //      * => If the name member of keyAlgorithm is "ML-KEM-768":
        //           Set the privateKey field to the result of DER-encoding a ML-KEM-65-PrivateKey
        //           ASN.1 type that represents the ML-KEM private key seed represented by the
        //           [[handle]] internal slot of key using the seed-only format (using a context-
        //           specific [0] primitive tag with an implicit encoding of OCTET STRING).
        //      * => If the name member of keyAlgorithm is "ML-KEM-1024":
        //           Set the privateKey field to the result of DER-encoding a ML-KEM-1024-PrivateKey
        //           ASN.1 type that represents the ML-KEM private key seed represented by the
        //           [[handle]] internal slot of key using the seed-only format (using a context-
        //           specific [0] primitive tag with an implicit encoding of OCTET STRING).
        //      * => Otherwise:
        //           throw a NotSupportedError.
        Array<u32, 9> algorithm_oid {};
        if (key->algorithm_name() == "ML-KEM-512"sv) {
            algorithm_oid = ::Crypto::ASN1::ml_kem_512_oid;
        } else if (key->algorithm_name() == "ML-KEM-768"sv) {
            algorithm_oid = ::Crypto::ASN1::ml_kem_768_oid;
        } else if (key->algorithm_name() == "ML-KEM-1024"sv) {
            algorithm_oid = ::Crypto::ASN1::ml_kem_1024_oid;
        } else {
            return WebIDL::NotSupportedError::create("Invalid algorithm"_utf16);
        }

        ::Crypto::ASN1::Encoder encoder;
        VERIFY(handle.has<::Crypto::PK::MLKEMPrivateKey>());
        auto const data = TRY_OR_THROW_OOM(realm.vm(), ::Crypto::PK::wrap_in_private_key_info(handle.get<::Crypto::PK::MLKEMPrivateKey>(), algorithm_oid));

        // 4. Let result be the result of DER-encoding data.
        result = ExportKeyResult { data };
    }
    //   -> If format is "raw-public":
    else if (format == KeyFormat::RawPublic) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != KeyType::Public)
            return WebIDL::InvalidAccessError::create("Key is not a public key"_utf16);

        // 2. Let data be a byte sequence containing the raw octets of the key represented by the [[handle]] internal slot of key.
        VERIFY(handle.has<::Crypto::PK::MLKEMPublicKey>());
        auto const data = handle.get<::Crypto::PK::MLKEMPublicKey>().public_key();

        // 3. Let result be data.
        result = ExportKeyResult { data };
    }
    //   -> If format is "raw-seed":
    else if (format == KeyFormat::RawSeed) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != KeyType::Private)
            return WebIDL::InvalidAccessError::create("Key is not a private key"_utf16);

        // 2. Let data be a byte sequence containing the concatenation of the d and z seed variables of the
        //    key represented by the [[handle]] internal slot of key.
        VERIFY(key->handle().has<::Crypto::PK::MLKEMPrivateKey>());
        auto const data = key->handle().get<::Crypto::PK::MLKEMPrivateKey>().seed();

        // 3. Let result be data.
        result = ExportKeyResult { data };
    }
    // FIXME:  -> If format is "jwk":
    //   -> Otherwise:
    else {
        // throw a NotSupportedError.
        return WebIDL::NotSupportedError::create("Invalid key format"_utf16);
    }

    // 4. Return result.
    return result.release_value();
}

Optional<ReadonlySpan<KeyUsage>> MLKEM::public_key_usages() const
{
    static constexpr Array usages { KeyUsage::EncapsulateKey, KeyUsage::EncapsulateBits };
    return usages.span();
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-kem-operations-encapsulate
WebIDL::ExceptionOr<EncapsulatedBits> MLKEM::encapsulate(JS::Realm&, AlgorithmParams const& params, GC::Ref<CryptoKey> key)
{
    // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
    if (key->type() != KeyType::Public)
        return WebIDL::InvalidAccessError::create("Invalid key type"_utf16);

    // 2. Perform the encapsulation key check described in Section 7.2 of [FIPS-203] with the parameter set indicated
    //    by the name member of algorithm, using the key represented by the [[handle]] internal slot of key as the ek
    //    input parameter.
    // 3. If the encapsulation key check failed, return an OperationError.
    // NOTE: Presumably done by OpenSSL for us

    // 4. Let sharedKey and ciphertext be the outputs that result from performing the ML-KEM.Encaps function
    //    described in Section 7.2 of [FIPS-203] with the parameter set indicated by the name member of algorithm,
    //    using the key represented by the [[handle]] internal slot of key as the ek input parameter.
    VERIFY(key->handle().has<::Crypto::PK::MLKEMPublicKey>());
    auto maybe_encapsulation = [&]() {
        if (params.name == "ML-KEM-512"sv) {
            return ::Crypto::PK::MLKEM::encapsulate(::Crypto::PK::MLKEMSize::MLKEM512, key->handle().get<::Crypto::PK::MLKEMPublicKey>());
        }
        if (params.name == "ML-KEM-768"sv) {
            return ::Crypto::PK::MLKEM::encapsulate(::Crypto::PK::MLKEMSize::MLKEM768, key->handle().get<::Crypto::PK::MLKEMPublicKey>());
        }
        if (params.name == "ML-KEM-1024"sv) {
            return ::Crypto::PK::MLKEM::encapsulate(::Crypto::PK::MLKEMSize::MLKEM1024, key->handle().get<::Crypto::PK::MLKEMPublicKey>());
        }
        VERIFY_NOT_REACHED();
    }();

    // 5. If the ML-KEM.Encaps function returned an error, return an OperationError.
    if (maybe_encapsulation.is_error())
        return WebIDL::OperationError::create(Utf16String::formatted("Key encapsulation failed: {}", maybe_encapsulation.release_error()));
    auto const [shared_key, ciphertext] = maybe_encapsulation.release_value();

    // 6. Let result be a new EncapsulatedBits dictionary.
    // 7. Set the sharedKey attribute of result to the result of creating an ArrayBuffer containing sharedKey.
    // 8. Set the ciphertext attribute of result to the result of creating an ArrayBuffer containing ciphertext.
    auto result = EncapsulatedBits { shared_key, ciphertext };

    // 9. Return result.
    return result;
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-kem-operations-decapsulate
WebIDL::ExceptionOr<ByteBuffer> MLKEM::decapsulate(JS::Realm&, AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& ciphertext)
{
    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != KeyType::Private)
        return WebIDL::InvalidAccessError::create("Invalid key type"_utf16);

    // 2. Perform the decapsulation input check described in Section 7.3 of [FIPS-203] with the parameter set
    //    indicated by the name member of algorithm, using the key represented by the [[handle]] internal slot of key as
    //    the dk input parameter, and ciphertext as the c input parameter.
    // 3. If the decapsulation key check failed, return an OperationError.
    // NOTE: Presumably done by OpenSSL for us

    // 4. Let sharedKey be the output that results from performing the ML-KEM.Decaps function described in Section
    //    7.3 of [FIPS-203] with the parameter set indicated by the name member of algorithm, using the key
    //    represented by the [[handle]] internal slot of key as the dk input parameter, and ciphertext as the c input
    //    parameter.
    VERIFY(key->handle().has<::Crypto::PK::MLKEMPrivateKey>());
    auto maybe_shared_key = [&]() {
        if (params.name == "ML-KEM-512"sv) {
            return ::Crypto::PK::MLKEM::decapsulate(::Crypto::PK::MLKEMSize::MLKEM512, key->handle().get<::Crypto::PK::MLKEMPrivateKey>(), ciphertext);
        }
        if (params.name == "ML-KEM-768"sv) {
            return ::Crypto::PK::MLKEM::decapsulate(::Crypto::PK::MLKEMSize::MLKEM768, key->handle().get<::Crypto::PK::MLKEMPrivateKey>(), ciphertext);
        }
        if (params.name == "ML-KEM-1024"sv) {
            return ::Crypto::PK::MLKEM::decapsulate(::Crypto::PK::MLKEMSize::MLKEM1024, key->handle().get<::Crypto::PK::MLKEMPrivateKey>(), ciphertext);
        }
        VERIFY_NOT_REACHED();
    }();

    if (maybe_shared_key.is_error())
        return WebIDL::OperationError::create(Utf16String::formatted("Key decapsulation failed: {}", maybe_shared_key.release_error()));

    auto const shared_key = maybe_shared_key.release_value();

    // 5. Return sharedKey.
    return shared_key;
}

// https://wicg.github.io/webcrypto-modern-algos/#ml-kem-operations-get-shared-key-length
WebIDL::ExceptionOr<u32> MLKEM::get_shared_key_length(AlgorithmParams const&) const
{
    // 1. Return 256.
    return 256;
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
size_t web_crypto_hash_digest_size(::Crypto::Hash::HashKind);
bool web_crypto_hkdf_derive_key(::Crypto::Hash::HashKind, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, u32, void*, ReceiveEncodedBytes);
bool web_crypto_pbkdf2_derive_key(::Crypto::Hash::HashKind, ReadonlyBytes, ReadonlyBytes, u32, u32, void*, ReceiveEncodedBytes);
FlatPtr web_crypto_argon2_validate_parameters(ReadonlyBytes);
FlatPtr web_crypto_argon2_derive_key(::Crypto::Hash::Argon2Type, Argon2Inputs const*, void*, ReceiveEncodedBytes);
}

size_t web_crypto_hash_digest_size(::Crypto::Hash::HashKind hash_kind)
{
    return ::Crypto::Hash::Manager { hash_kind }.digest_size();
}

bool web_crypto_hkdf_derive_key(::Crypto::Hash::HashKind kind, ReadonlyBytes salt, ReadonlyBytes key_derivation_key, ReadonlyBytes info, u32 length, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Hash::HKDF hkdf(kind);
    auto result = hkdf.derive_key(salt, key_derivation_key, info, length);
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

bool web_crypto_pbkdf2_derive_key(::Crypto::Hash::HashKind kind, ReadonlyBytes password, ReadonlyBytes salt, u32 iterations, u32 length, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Hash::PBKDF2 pbkdf2(kind);
    auto result = pbkdf2.derive_key(password, salt, iterations, length);
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

static_assert(to_underlying(::Crypto::Hash::Argon2Type::Argon2d) == 0);
static_assert(to_underlying(::Crypto::Hash::Argon2Type::Argon2i) == 1);
static_assert(to_underlying(::Crypto::Hash::Argon2Type::Argon2id) == 2);

FlatPtr web_crypto_argon2_validate_parameters(ReadonlyBytes nonce)
{
    auto result = ::Crypto::Hash::Argon2::validate_parameters(nonce);
    if (result.is_error())
        return Utf16String::formatted("{}", result.error()).into_raw();
    return 0;
}

FlatPtr web_crypto_argon2_derive_key(::Crypto::Hash::Argon2Type kind, Argon2Inputs const* inputs, void* state, ReceiveEncodedBytes receive)
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

extern "C" FlatPtr web_crypto_cshake_validate_parameters(u32, OptionalBytes, OptionalBytes);
extern "C" FlatPtr web_crypto_cshake_digest(::Crypto::Hash::SHAKEKind, ReadonlyBytes, u32, OptionalBytes, OptionalBytes, void*, ReceiveEncodedBytes);

FlatPtr web_crypto_cshake_validate_parameters(u32 output_length, OptionalBytes customization, OptionalBytes function_name)
{
    auto result = ::Crypto::Hash::SHAKE::validate_parameters(output_length, customization.to_optional(), function_name.to_optional());
    if (result.is_error())
        return Utf16String::formatted("{}", result.error()).into_raw();
    return 0;
}

FlatPtr web_crypto_cshake_digest(::Crypto::Hash::SHAKEKind kind, ReadonlyBytes data, u32 output_length, OptionalBytes customization, OptionalBytes function_name, void* state, ReceiveEncodedBytes receive)
{
    auto maybe_result = ::Crypto::Hash::SHAKE(kind).digest(data, output_length, customization.to_optional(), function_name.to_optional());
    if (maybe_result.is_error())
        return Utf16String::formatted("{}", maybe_result.error()).into_raw();
    receive(state, maybe_result.value());
    return 0;
}

extern "C" FlatPtr web_crypto_chacha20_poly1305_cipher(bool, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, ReadonlyBytes, void*, ReceiveEncodedBytes);

FlatPtr web_crypto_chacha20_poly1305_cipher(bool encrypt, ReadonlyBytes key, ReadonlyBytes iv, ReadonlyBytes input, ReadonlyBytes additional_data, void* state, ReceiveEncodedBytes receive)
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
u32 web_crypto_kmac_default_key_length(::Crypto::Authentication::KMACKind);
bool web_crypto_kmac_validate_parameters(u32, OptionalBytes);
bool web_crypto_kmac_sign(::Crypto::Authentication::KMACKind, ReadonlyBytes, ReadonlyBytes, u32, OptionalBytes, void*, ReceiveEncodedBytes);
}

static_assert(to_underlying(::Crypto::Authentication::KMACKind::KMAC128) == 0);
static_assert(to_underlying(::Crypto::Authentication::KMACKind::KMAC256) == 1);

u32 web_crypto_kmac_default_key_length(::Crypto::Authentication::KMACKind kind)
{
    return ::Crypto::Authentication::KMAC::default_key_length(kind);
}

bool web_crypto_kmac_validate_parameters(u32 output_length, OptionalBytes customization)
{
    return !::Crypto::Authentication::KMAC::validate_parameters(output_length, customization.to_optional()).is_error();
}

bool web_crypto_kmac_sign(::Crypto::Authentication::KMACKind kind, ReadonlyBytes key, ReadonlyBytes message, u32 output_length, OptionalBytes customization, void* state, ReceiveEncodedBytes receive)
{
    ::Crypto::Authentication::KMAC kmac(kind);
    auto result = kmac.sign(key, message, output_length, customization.to_optional());
    if (result.is_error())
        return false;
    receive(state, result.value());
    return true;
}

}
