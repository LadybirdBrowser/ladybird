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
#include <AK/QuickSort.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/ASN1/Constants.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Authentication/HMAC.h>
#include <LibCrypto/Certificate/Certificate.h>
#include <LibCrypto/Cipher/AES.h>
#include <LibCrypto/Curves/EdwardsCurve.h>
#include <LibCrypto/Curves/SECPxxxr1.h>
#include <LibCrypto/Hash/HKDF.h>
#include <LibCrypto/Hash/HashManager.h>
#include <LibCrypto/Hash/PBKDF2.h>
#include <LibCrypto/Hash/SHA1.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibCrypto/PK/RSA.h>
#include <LibCrypto/SecureRandom.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Crypto/CryptoAlgorithms.h>
#include <LibWeb/Crypto/KeyAlgorithms.h>
#include <LibWeb/Crypto/SubtleCrypto.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::Crypto {

static JS::ThrowCompletionOr<HashAlgorithmIdentifier> hash_algorithm_identifier_from_value(JS::VM& vm, JS::Value hash_value)
{
    auto* realm = vm.current_realm();

    auto maybe_normalized_algorithm = [&]() -> WebIDL::ExceptionOr<NormalizedAlgorithmAndParameter> {
        if (hash_value.is_string()) {
            auto hash_string = TRY(hash_value.to_string(vm));
            return normalize_an_algorithm(*realm, hash_string, "digest"_string);
        } else if (hash_value.is_object()) {
            auto hash_object = TRY(hash_value.to_object(vm));
            auto hash_object_root = GC::make_root(hash_object);
            return normalize_an_algorithm(*realm, hash_object_root, "digest"_string);
        } else {
            VERIFY_NOT_REACHED();
        }
    }();

    if (maybe_normalized_algorithm.is_error()) {
        return Bindings::exception_to_throw_completion(vm, maybe_normalized_algorithm.exception());
    }

    return HashAlgorithmIdentifier { maybe_normalized_algorithm.value().parameter->name };
}

// https://w3c.github.io/webcrypto/#concept-usage-intersection
static Vector<Bindings::KeyUsage> usage_intersection(ReadonlySpan<Bindings::KeyUsage> a, ReadonlySpan<Bindings::KeyUsage> b)
{
    Vector<Bindings::KeyUsage> result;
    for (auto const& usage : a) {
        if (b.contains_slow(usage))
            result.append(usage);
    }
    quick_sort(result);
    return result;
}

// Out of line to ensure this class has a key function
AlgorithmMethods::~AlgorithmMethods() = default;

// https://w3c.github.io/webcrypto/#big-integer
static ::Crypto::UnsignedBigInteger big_integer_from_api_big_integer(GC::Ptr<JS::Uint8Array> const& big_integer)
{
    // The BigInteger typedef is a Uint8Array that holds an arbitrary magnitude unsigned integer
    // **in big-endian order**. Values read from the API SHALL have minimal typed array length
    // (that is, at most 7 leading zero bits, except the value 0 which shall have length 8 bits).
    // The API SHALL accept values with any number of leading zero bits, including the empty array, which represents zero.

    auto const& buffer = big_integer->viewed_array_buffer()->buffer();

    ::Crypto::UnsignedBigInteger result(0);
    if (buffer.size() > 0) {
        if constexpr (AK::HostIsLittleEndian) {
            // We need to reverse the buffer to get it into little-endian order
            Vector<u8, 32> reversed_buffer;
            reversed_buffer.resize(buffer.size());
            for (size_t i = 0; i < buffer.size(); ++i) {
                reversed_buffer[buffer.size() - i - 1] = buffer[i];
            }

            return ::Crypto::UnsignedBigInteger::import_data(reversed_buffer.data(), reversed_buffer.size());
        } else {
            return ::Crypto::UnsignedBigInteger::import_data(buffer.data(), buffer.size());
        }
    }
    return ::Crypto::UnsignedBigInteger(0);
}

// https://www.rfc-editor.org/rfc/rfc7518#section-2
ErrorOr<String> base64_url_uint_encode(::Crypto::UnsignedBigInteger integer)
{
    // The representation of a positive or zero integer value as the
    // base64url encoding of the value's unsigned big-endian
    // representation as an octet sequence.  The octet sequence MUST
    // utilize the minimum number of octets needed to represent the
    // value.  Zero is represented as BASE64URL(single zero-valued
    // octet), which is "AA".

    auto bytes = TRY(ByteBuffer::create_uninitialized(integer.trimmed_byte_length()));

    bool const remove_leading_zeroes = true;
    auto data_size = integer.export_data(bytes.span(), remove_leading_zeroes);

    auto data_slice_be = bytes.bytes().slice(bytes.size() - data_size, data_size);

    String encoded;
    if constexpr (AK::HostIsLittleEndian) {
        // We need to encode the integer's big endian representation as a base64 string
        Vector<u8, 32> data_slice_cpu;
        data_slice_cpu.ensure_capacity(data_size);
        for (size_t i = 0; i < data_size; ++i) {
            data_slice_cpu.append(data_slice_be[data_size - i - 1]);
        }
        encoded = TRY(encode_base64url(data_slice_cpu));
    } else {
        encoded = TRY(encode_base64url(data_slice_be));
    }

    // FIXME: create a version of encode_base64url that omits padding bytes
    if (auto first_padding_byte = encoded.find_byte_offset('='); first_padding_byte.has_value())
        return encoded.substring_from_byte_offset(0, first_padding_byte.value());
    return encoded;
}

WebIDL::ExceptionOr<ByteBuffer> base64_url_bytes_decode(JS::Realm& realm, String const& base64_url_string)
{
    auto& vm = realm.vm();

    // FIXME: Create a version of decode_base64url that ignores padding inconsistencies
    auto padded_string = base64_url_string;
    if (padded_string.byte_count() % 4 != 0) {
        padded_string = TRY_OR_THROW_OOM(vm, String::formatted("{}{}", padded_string, TRY_OR_THROW_OOM(vm, String::repeated('=', 4 - (padded_string.byte_count() % 4)))));
    }

    auto base64_bytes_or_error = decode_base64url(padded_string);
    if (base64_bytes_or_error.is_error()) {
        if (base64_bytes_or_error.error().code() == ENOMEM)
            return vm.throw_completion<JS::InternalError>(vm.error_message(::JS::VM::ErrorMessage::OutOfMemory));
        return WebIDL::DataError::create(realm, MUST(String::formatted("base64 decode: {}", base64_bytes_or_error.release_error())));
    }
    return base64_bytes_or_error.release_value();
}

WebIDL::ExceptionOr<::Crypto::UnsignedBigInteger> base64_url_uint_decode(JS::Realm& realm, String const& base64_url_string)
{
    auto base64_bytes_be = TRY(base64_url_bytes_decode(realm, base64_url_string));

    if constexpr (AK::HostIsLittleEndian) {
        // We need to swap the integer's big-endian representation to little endian in order to import it
        Vector<u8, 32> base64_bytes_cpu;
        base64_bytes_cpu.ensure_capacity(base64_bytes_be.size());
        for (size_t i = 0; i < base64_bytes_be.size(); ++i) {
            base64_bytes_cpu.append(base64_bytes_be[base64_bytes_be.size() - i - 1]);
        }
        return ::Crypto::UnsignedBigInteger::import_data(base64_bytes_cpu.data(), base64_bytes_cpu.size());
    } else {
        return ::Crypto::UnsignedBigInteger::import_data(base64_bytes_be.data(), base64_bytes_be.size());
    }
}

// https://w3c.github.io/webcrypto/#concept-parse-an-asn1-structure
template<typename Structure>
static WebIDL::ExceptionOr<Structure> parse_an_ASN1_structure(JS::Realm& realm, ReadonlyBytes data, bool exact_data = true)
{
    // 1. Let data be a sequence of bytes to be parsed.
    // 2. Let structure be the ASN.1 structure to be parsed.
    // 3. Let exactData be an optional boolean value. If it is not supplied, let it be initialized to true.

    // 4. Parse data according to the Distinguished Encoding Rules of [X690], using structure as the ASN.1 structure to be decoded.
    ::Crypto::ASN1::Decoder decoder(data);
    Structure structure;
    if constexpr (IsSame<Structure, ::Crypto::Certificate::SubjectPublicKey>) {
        auto maybe_subject_public_key = ::Crypto::Certificate::parse_subject_public_key_info(decoder);
        if (maybe_subject_public_key.is_error())
            return WebIDL::DataError::create(realm, MUST(String::formatted("Error parsing subjectPublicKeyInfo: {}", maybe_subject_public_key.release_error())));
        structure = maybe_subject_public_key.release_value();
    } else if constexpr (IsSame<Structure, ::Crypto::Certificate::PrivateKey>) {
        auto maybe_private_key = ::Crypto::Certificate::parse_private_key_info(decoder);
        if (maybe_private_key.is_error())
            return WebIDL::DataError::create(realm, MUST(String::formatted("Error parsing privateKeyInfo: {}", maybe_private_key.release_error())));
        structure = maybe_private_key.release_value();
    } else if constexpr (IsSame<Structure, StringView>) {
        auto read_result = decoder.read<StringView>(::Crypto::ASN1::Class::Universal, ::Crypto::ASN1::Kind::OctetString);
        if (read_result.is_error())
            return WebIDL::DataError::create(realm, MUST(String::formatted("Read of kind OctetString failed: {}", read_result.error())));
        structure = read_result.release_value();
    } else {
        static_assert(DependentFalse<Structure>, "Don't know how to parse ASN.1 structure type");
    }

    // 5. If exactData was specified, and all of the bytes of data were not consumed during the parsing phase, then throw a DataError.
    if (exact_data && !decoder.eof())
        return WebIDL::DataError::create(realm, "Not all bytes were consumed during the parsing phase"_string);

    // 6. Return the parsed ASN.1 structure.
    return structure;
}

// https://w3c.github.io/webcrypto/#concept-parse-a-spki
static WebIDL::ExceptionOr<::Crypto::Certificate::SubjectPublicKey> parse_a_subject_public_key_info(JS::Realm& realm, ReadonlyBytes bytes)
{
    // When this specification says to parse a subjectPublicKeyInfo, the user agent must parse an ASN.1 structure,
    // with data set to the sequence of bytes to be parsed, structure as the ASN.1 structure of subjectPublicKeyInfo,
    // as specified in [RFC5280], and exactData set to true.
    return parse_an_ASN1_structure<::Crypto::Certificate::SubjectPublicKey>(realm, bytes, true);
}

// https://w3c.github.io/webcrypto/#concept-parse-a-privateKeyInfo
static WebIDL::ExceptionOr<::Crypto::Certificate::PrivateKey> parse_a_private_key_info(JS::Realm& realm, ReadonlyBytes bytes)
{
    // When this specification says to parse a PrivateKeyInfo, the user agent must parse an ASN.1 structure
    // with data set to the sequence of bytes to be parsed, structure as the ASN.1 structure of PrivateKeyInfo,
    // as specified in [RFC5208], and exactData set to true.
    return parse_an_ASN1_structure<::Crypto::Certificate::PrivateKey>(realm, bytes, true);
}

static WebIDL::ExceptionOr<::Crypto::PK::RSAPrivateKey<>> parse_jwk_rsa_private_key(JS::Realm& realm, Bindings::JsonWebKey const& jwk)
{
    auto n = TRY(base64_url_uint_decode(realm, *jwk.n));
    auto d = TRY(base64_url_uint_decode(realm, *jwk.d));
    auto e = TRY(base64_url_uint_decode(realm, *jwk.e));

    // We know that if any of the extra parameters are provided, all of them must be
    if (!jwk.p.has_value())
        return ::Crypto::PK::RSAPrivateKey<>(move(n), move(d), move(e));

    auto p = TRY(base64_url_uint_decode(realm, *jwk.p));
    auto q = TRY(base64_url_uint_decode(realm, *jwk.q));
    auto dp = TRY(base64_url_uint_decode(realm, *jwk.dp));
    auto dq = TRY(base64_url_uint_decode(realm, *jwk.dq));
    auto qi = TRY(base64_url_uint_decode(realm, *jwk.qi));

    return ::Crypto::PK::RSAPrivateKey<>(move(n), move(d), move(e), move(p), move(q), move(dp), move(dq), move(qi));
}

static WebIDL::ExceptionOr<::Crypto::PK::RSAPublicKey<>> parse_jwk_rsa_public_key(JS::Realm& realm, Bindings::JsonWebKey const& jwk)
{
    auto e = TRY(base64_url_uint_decode(realm, *jwk.e));
    auto n = TRY(base64_url_uint_decode(realm, *jwk.n));

    return ::Crypto::PK::RSAPublicKey<>(move(n), move(e));
}

static WebIDL::ExceptionOr<ByteBuffer> parse_jwk_symmetric_key(JS::Realm& realm, Bindings::JsonWebKey const& jwk)
{
    if (!jwk.k.has_value()) {
        return WebIDL::DataError::create(realm, "JWK has no 'k' field"_string);
    }
    return base64_url_bytes_decode(realm, *jwk.k);
}

// https://www.rfc-editor.org/rfc/rfc7517#section-4.3
static WebIDL::ExceptionOr<void> validate_jwk_key_ops(JS::Realm& realm, Bindings::JsonWebKey const& jwk, Vector<Bindings::KeyUsage> const& usages)
{
    // Use of the "key_ops" member is OPTIONAL, unless the application requires its presence.
    if (!jwk.key_ops.has_value())
        return {};
    auto key_operations = *jwk.key_ops;

    // Duplicate key operation values MUST NOT be present in the array
    HashTable<String> seen_operations;
    for (auto const& key_operation : key_operations) {
        if (seen_operations.set(key_operation) != HashSetResult::InsertedNewEntry)
            return WebIDL::DataError::create(realm, MUST(String::formatted("Duplicate key operation: {}", key_operation)));
    }

    // Multiple unrelated key operations SHOULD NOT be specified for a key because of the potential
    // vulnerabilities associated with using the same key with multiple algorithms.  Thus, the
    // combinations "sign" with "verify", "encrypt" with "decrypt", and "wrapKey" with "unwrapKey"
    // are permitted, but other combinations SHOULD NOT be used.
    auto is_used_for_signing = seen_operations.contains("sign"sv) || seen_operations.contains("verify"sv);
    auto is_used_for_encryption = seen_operations.contains("encrypt"sv) || seen_operations.contains("decrypt"sv);
    auto is_used_for_wrapping = seen_operations.contains("wrapKey"sv) || seen_operations.contains("unwrapKey"sv);
    auto number_of_operation_types = is_used_for_signing + is_used_for_encryption + is_used_for_wrapping;
    if (number_of_operation_types > 1)
        return WebIDL::DataError::create(realm, "Multiple unrelated key operations are specified"_string);

    // The "use" and "key_ops" JWK members SHOULD NOT be used together; however, if both are used,
    // the information they convey MUST be consistent. Applications should specify which of these
    // members they use, if either is to be used by the application.
    if (jwk.use.has_value()) {
        for (auto const& key_operation : key_operations) {
            if (key_operation == "deriveKey"sv || key_operation == "deriveBits"sv)
                continue;
            if (jwk.use == "sig"sv && key_operation != "sign"sv && key_operation != "verify"sv)
                return WebIDL::DataError::create(realm, "use=sig but key_ops does not contain 'sign' or 'verify'"_string);
            if (jwk.use == "enc"sv && (key_operation == "sign"sv || key_operation == "verify"sv))
                return WebIDL::DataError::create(realm, "use=enc but key_ops contains 'sign' or 'verify'"_string);
        }
    }

    // NOTE: This validation happens in multiple places in the spec, so it is here for convenience.
    for (auto const& usage : usages) {
        if (!seen_operations.contains(Bindings::idl_enum_to_string(usage)))
            return WebIDL::DataError::create(realm, MUST(String::formatted("Missing key_ops usage: {}", Bindings::idl_enum_to_string(usage))));
    }

    return {};
}

static WebIDL::ExceptionOr<ByteBuffer> generate_random_key(JS::VM& vm, u16 const size_in_bits)
{
    auto key_buffer = TRY_OR_THROW_OOM(vm, ByteBuffer::create_uninitialized(size_in_bits / 8));
    ::Crypto::fill_with_secure_random(key_buffer);
    return key_buffer;
}

AlgorithmParams::~AlgorithmParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> AlgorithmParams::from_value(JS::VM&, JS::Value)
{
    return adopt_own(*new AlgorithmParams {});
}

AesCbcParams::~AesCbcParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> AesCbcParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto iv_value = TRY(object.get("iv"_fly_string));
    if (!iv_value.is_object() || !(is<JS::TypedArrayBase>(iv_value.as_object()) || is<JS::ArrayBuffer>(iv_value.as_object()) || is<JS::DataView>(iv_value.as_object())))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "BufferSource");
    auto iv = TRY_OR_THROW_OOM(vm, WebIDL::get_buffer_source_copy(iv_value.as_object()));

    return adopt_own<AlgorithmParams>(*new AesCbcParams { iv });
}

AesCtrParams::~AesCtrParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> AesCtrParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto iv_value = TRY(object.get("counter"_fly_string));
    if (!iv_value.is_object() || !(is<JS::TypedArrayBase>(iv_value.as_object()) || is<JS::ArrayBuffer>(iv_value.as_object()) || is<JS::DataView>(iv_value.as_object())))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "BufferSource");
    auto iv = TRY_OR_THROW_OOM(vm, WebIDL::get_buffer_source_copy(iv_value.as_object()));

    auto length_value = TRY(object.get("length"_fly_string));
    auto length = TRY(length_value.to_u8(vm));

    return adopt_own<AlgorithmParams>(*new AesCtrParams { iv, length });
}

AesGcmParams::~AesGcmParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> AesGcmParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto iv_value = TRY(object.get("iv"_fly_string));
    if (!iv_value.is_object() || !(is<JS::TypedArrayBase>(iv_value.as_object()) || is<JS::ArrayBuffer>(iv_value.as_object()) || is<JS::DataView>(iv_value.as_object())))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "BufferSource");
    auto iv = TRY_OR_THROW_OOM(vm, WebIDL::get_buffer_source_copy(iv_value.as_object()));

    auto maybe_additional_data = Optional<ByteBuffer> {};
    if (MUST(object.has_property("additionalData"_fly_string))) {
        auto additional_data_value = TRY(object.get("additionalData"_fly_string));
        if (!additional_data_value.is_object() || !(is<JS::TypedArrayBase>(additional_data_value.as_object()) || is<JS::ArrayBuffer>(additional_data_value.as_object()) || is<JS::DataView>(additional_data_value.as_object())))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "BufferSource");
        maybe_additional_data = TRY_OR_THROW_OOM(vm, WebIDL::get_buffer_source_copy(additional_data_value.as_object()));
    }

    auto maybe_tag_length = Optional<u8> {};
    if (MUST(object.has_property("tagLength"_fly_string))) {
        auto tag_length_value = TRY(object.get("tagLength"_fly_string));
        maybe_tag_length = TRY(tag_length_value.to_u8(vm));
    }

    return adopt_own<AlgorithmParams>(*new AesGcmParams { iv, maybe_additional_data, maybe_tag_length });
}

HKDFParams::~HKDFParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> HKDFParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto hash_value = TRY(object.get("hash"_fly_string));
    auto hash = TRY(hash_algorithm_identifier_from_value(vm, hash_value));

    auto salt_value = TRY(object.get("salt"_fly_string));
    if (!salt_value.is_object() || !(is<JS::TypedArrayBase>(salt_value.as_object()) || is<JS::ArrayBuffer>(salt_value.as_object()) || is<JS::DataView>(salt_value.as_object())))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "BufferSource");
    auto salt = TRY_OR_THROW_OOM(vm, WebIDL::get_buffer_source_copy(salt_value.as_object()));

    auto info_value = TRY(object.get("info"_fly_string));
    if (!info_value.is_object() || !(is<JS::TypedArrayBase>(info_value.as_object()) || is<JS::ArrayBuffer>(info_value.as_object()) || is<JS::DataView>(info_value.as_object())))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "BufferSource");
    auto info = TRY_OR_THROW_OOM(vm, WebIDL::get_buffer_source_copy(info_value.as_object()));

    return adopt_own<AlgorithmParams>(*new HKDFParams { hash, salt, info });
}

PBKDF2Params::~PBKDF2Params() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> PBKDF2Params::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto salt_value = TRY(object.get("salt"_fly_string));

    if (!salt_value.is_object() || !(is<JS::TypedArrayBase>(salt_value.as_object()) || is<JS::ArrayBuffer>(salt_value.as_object()) || is<JS::DataView>(salt_value.as_object())))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "BufferSource");

    auto salt = TRY_OR_THROW_OOM(vm, WebIDL::get_buffer_source_copy(salt_value.as_object()));

    auto iterations_value = TRY(object.get("iterations"_fly_string));
    auto iterations = TRY(iterations_value.to_u32(vm));

    auto hash_value = TRY(object.get("hash"_fly_string));
    auto hash = TRY(hash_algorithm_identifier_from_value(vm, hash_value));

    return adopt_own<AlgorithmParams>(*new PBKDF2Params { salt, iterations, hash });
}

RsaKeyGenParams::~RsaKeyGenParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> RsaKeyGenParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto modulus_length_value = TRY(object.get("modulusLength"_fly_string));
    auto modulus_length = TRY(modulus_length_value.to_u32(vm));

    auto public_exponent_value = TRY(object.get("publicExponent"_fly_string));
    GC::Ptr<JS::Uint8Array> public_exponent;

    if (!public_exponent_value.is_object() || !is<JS::Uint8Array>(public_exponent_value.as_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Uint8Array");

    public_exponent = static_cast<JS::Uint8Array&>(public_exponent_value.as_object());

    return adopt_own<AlgorithmParams>(*new RsaKeyGenParams { modulus_length, big_integer_from_api_big_integer(public_exponent) });
}

RsaHashedKeyGenParams::~RsaHashedKeyGenParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> RsaHashedKeyGenParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto modulus_length_value = TRY(object.get("modulusLength"_fly_string));
    auto modulus_length = TRY(modulus_length_value.to_u32(vm));

    auto public_exponent_value = TRY(object.get("publicExponent"_fly_string));
    GC::Ptr<JS::Uint8Array> public_exponent;

    if (!public_exponent_value.is_object() || !is<JS::Uint8Array>(public_exponent_value.as_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Uint8Array");

    public_exponent = static_cast<JS::Uint8Array&>(public_exponent_value.as_object());

    auto hash_value = TRY(object.get("hash"_fly_string));
    auto hash = TRY(hash_algorithm_identifier_from_value(vm, hash_value));

    return adopt_own<AlgorithmParams>(*new RsaHashedKeyGenParams { modulus_length, big_integer_from_api_big_integer(public_exponent), hash });
}

RsaHashedImportParams::~RsaHashedImportParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> RsaHashedImportParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto hash_value = TRY(object.get("hash"_fly_string));
    auto hash = TRY(hash_algorithm_identifier_from_value(vm, hash_value));

    return adopt_own<AlgorithmParams>(*new RsaHashedImportParams { hash });
}

RsaOaepParams::~RsaOaepParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> RsaOaepParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto label_value = TRY(object.get("label"_fly_string));

    ByteBuffer label;
    if (!label_value.is_nullish()) {
        if (!label_value.is_object() || !(is<JS::TypedArrayBase>(label_value.as_object()) || is<JS::ArrayBuffer>(label_value.as_object()) || is<JS::DataView>(label_value.as_object())))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "BufferSource");

        label = TRY_OR_THROW_OOM(vm, WebIDL::get_buffer_source_copy(label_value.as_object()));
    }

    return adopt_own<AlgorithmParams>(*new RsaOaepParams { move(label) });
}

RsaPssParams::~RsaPssParams() = default;

// https://w3c.github.io/webcrypto/#RsaPssParams-dictionary
JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> RsaPssParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto salt_length_value = TRY(object.get("saltLength"_fly_string));
    auto salt_length = TRY(salt_length_value.to_u32(vm));

    return adopt_own<AlgorithmParams>(*new RsaPssParams { salt_length });
}

EcdsaParams::~EcdsaParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> EcdsaParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto hash_value = TRY(object.get("hash"_fly_string));
    auto hash = TRY(hash_algorithm_identifier_from_value(vm, hash_value));

    return adopt_own<AlgorithmParams>(*new EcdsaParams { hash });
}

EcKeyGenParams::~EcKeyGenParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> EcKeyGenParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto curve_value = TRY(object.get("namedCurve"_fly_string));
    auto curve = TRY(curve_value.to_string(vm));

    return adopt_own<AlgorithmParams>(*new EcKeyGenParams { curve });
}

AesKeyGenParams::~AesKeyGenParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> AesKeyGenParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto length_value = TRY(object.get("length"_fly_string));
    auto length = TRY(length_value.to_u16(vm));

    return adopt_own<AlgorithmParams>(*new AesKeyGenParams { length });
}

AesDerivedKeyParams::~AesDerivedKeyParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> AesDerivedKeyParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto length_value = TRY(object.get("length"_fly_string));
    auto length = TRY(length_value.to_u16(vm));

    return adopt_own<AlgorithmParams>(*new AesDerivedKeyParams { length });
}

EcdhKeyDeriveParams::~EcdhKeyDeriveParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> EcdhKeyDeriveParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto key_value = TRY(object.get("public"_fly_string));
    auto key_object = TRY(key_value.to_object(vm));

    if (!is<CryptoKey>(*key_object)) {
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "CryptoKey");
    }

    auto& key = as<CryptoKey>(*key_object);

    return adopt_own<AlgorithmParams>(*new EcdhKeyDeriveParams { key });
}

EcKeyImportParams::~EcKeyImportParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> EcKeyImportParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto named_curve_value = TRY(object.get("namedCurve"_fly_string));
    auto named_curve = TRY(named_curve_value.to_string(vm));

    return adopt_own<AlgorithmParams>(*new EcKeyImportParams { named_curve });
}

HmacImportParams::~HmacImportParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> HmacImportParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto hash_value = TRY(object.get("hash"_fly_string));
    auto hash = TRY(hash_algorithm_identifier_from_value(vm, hash_value));

    auto maybe_length = Optional<WebIDL::UnsignedLong> {};
    if (MUST(object.has_property("length"_fly_string))) {
        auto length_value = TRY(object.get("length"_fly_string));
        maybe_length = TRY(length_value.to_u32(vm));
    }

    return adopt_own<AlgorithmParams>(*new HmacImportParams { hash, maybe_length });
}

HmacKeyGenParams::~HmacKeyGenParams() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> HmacKeyGenParams::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto hash_value = TRY(object.get("hash"_fly_string));
    auto hash = TRY(hash_algorithm_identifier_from_value(vm, hash_value));

    auto maybe_length = Optional<WebIDL::UnsignedLong> {};
    if (MUST(object.has_property("length"_fly_string))) {
        auto length_value = TRY(object.get("length"_fly_string));
        maybe_length = TRY(length_value.to_u32(vm));
    }

    return adopt_own<AlgorithmParams>(*new HmacKeyGenParams { hash, maybe_length });
}

Ed448Params::~Ed448Params() = default;

JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> Ed448Params::from_value(JS::VM& vm, JS::Value value)
{
    auto& object = value.as_object();

    auto maybe_context = Optional<ByteBuffer> {};
    if (MUST(object.has_property("context"_fly_string))) {
        auto context_value = TRY(object.get("context"_fly_string));
        if (!context_value.is_object() || !(is<JS::TypedArrayBase>(context_value.as_object()) || is<JS::ArrayBuffer>(context_value.as_object()) || is<JS::DataView>(context_value.as_object())))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "BufferSource");
        maybe_context = TRY_OR_THROW_OOM(vm, WebIDL::get_buffer_source_copy(context_value.as_object()));
    }

    return adopt_own<AlgorithmParams>(*new Ed448Params { maybe_context });
}

// https://w3c.github.io/webcrypto/#rsa-oaep-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> RSAOAEP::encrypt(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& plaintext)
{
    auto& realm = *m_realm;
    auto& vm = realm.vm();
    auto const& normalized_algorithm = static_cast<RsaOaepParams const&>(params);

    // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Public)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a public key"_string);

    // 2. Let label be the contents of the label member of normalizedAlgorithm or the empty octet string if the label member of normalizedAlgorithm is not present.
    auto const& label = normalized_algorithm.label;

    auto const& handle = key->handle();
    auto public_key = handle.get<::Crypto::PK::RSAPublicKey<>>();
    auto hash = TRY(as<RsaHashedKeyAlgorithm>(*key->algorithm()).hash().name(vm));

    // 3. Perform the encryption operation defined in Section 7.1 of [RFC3447] with the key represented by key as the recipient's RSA public key,
    //    the contents of plaintext as the message to be encrypted, M and label as the label, L, and with the hash function specified by the hash attribute
    //    of the [[algorithm]] internal slot of key as the Hash option and MGF1 (defined in Section B.2.1 of [RFC3447]) as the MGF option.
    Optional<::Crypto::Hash::HashKind> hash_kind = {};
    if (hash == "SHA-1")
        hash_kind = ::Crypto::Hash::HashKind::SHA1;
    else if (hash == "SHA-256")
        hash_kind = ::Crypto::Hash::HashKind::SHA256;
    else if (hash == "SHA-384")
        hash_kind = ::Crypto::Hash::HashKind::SHA384;
    else if (hash == "SHA-512")
        hash_kind = ::Crypto::Hash::HashKind::SHA512;

    // 4. If performing the operation results in an error, then throw an OperationError.
    if (!hash_kind.has_value()) {
        auto error_message = MUST(String::formatted("Invalid hash function '{}'", hash));
        return WebIDL::OperationError::create(realm, error_message);
    }

    // 5. Let ciphertext be the value C that results from performing the operation.
    auto rsa = ::Crypto::PK::RSA_OAEP_EME { *hash_kind, public_key };
    rsa.set_label(label);

    auto maybe_ciphertext = rsa.encrypt(plaintext);
    if (maybe_ciphertext.is_error())
        return WebIDL::OperationError::create(realm, "Failed to encrypt"_string);

    // 6. Return the result of creating an ArrayBuffer containing ciphertext.
    return JS::ArrayBuffer::create(realm, maybe_ciphertext.release_value());
}

// https://w3c.github.io/webcrypto/#rsa-oaep-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> RSAOAEP::decrypt(AlgorithmParams const& params, GC::Ref<CryptoKey> key, AK::ByteBuffer const& ciphertext)
{
    auto& realm = *m_realm;
    auto& vm = realm.vm();
    auto const& normalized_algorithm = static_cast<RsaOaepParams const&>(params);

    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Private)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a private key"_string);

    // 2. Let label be the contents of the label member of normalizedAlgorithm or the empty octet string if the label member of normalizedAlgorithm is not present.
    auto const& label = normalized_algorithm.label;

    auto const& handle = key->handle();
    auto private_key = handle.get<::Crypto::PK::RSAPrivateKey<>>();
    auto hash = TRY(as<RsaHashedKeyAlgorithm>(*key->algorithm()).hash().name(vm));

    // 3. Perform the decryption operation defined in Section 7.1 of [RFC3447] with the key represented by key as the recipient's RSA private key,
    //    the contents of ciphertext as the ciphertext to be decrypted, C, and label as the label, L, and with the hash function specified by the hash attribute
    //    of the [[algorithm]] internal slot of key as the Hash option and MGF1 (defined in Section B.2.1 of [RFC3447]) as the MGF option.
    Optional<::Crypto::Hash::HashKind> hash_kind = {};
    if (hash == "SHA-1")
        hash_kind = ::Crypto::Hash::HashKind::SHA1;
    else if (hash == "SHA-256")
        hash_kind = ::Crypto::Hash::HashKind::SHA256;
    else if (hash == "SHA-384")
        hash_kind = ::Crypto::Hash::HashKind::SHA384;
    else if (hash == "SHA-512")
        hash_kind = ::Crypto::Hash::HashKind::SHA512;

    // 4. If performing the operation results in an error, then throw an OperationError.
    if (!hash_kind.has_value()) {
        auto error_message = MUST(String::formatted("Invalid hash function '{}'", hash));
        return WebIDL::OperationError::create(realm, error_message);
    }

    // 5. Let plaintext the value M that results from performing the operation.
    auto rsa = ::Crypto::PK::RSA_OAEP_EME { *hash_kind, private_key };
    rsa.set_label(label);

    auto maybe_plaintext = rsa.decrypt(ciphertext);
    if (maybe_plaintext.is_error())
        return WebIDL::OperationError::create(realm, "Failed to encrypt"_string);

    // 6. Return the result of creating an ArrayBuffer containing plaintext.
    return JS::ArrayBuffer::create(realm, maybe_plaintext.release_value());
}

// https://w3c.github.io/webcrypto/#rsa-oaep-operations
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> RSAOAEP::generate_key(AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains an entry which is not "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Encrypt && usage != Bindings::KeyUsage::Decrypt && usage != Bindings::KeyUsage::Wrapkey && usage != Bindings::KeyUsage::Unwrapkey) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    // 2. Generate an RSA key pair, as defined in [RFC3447], with RSA modulus length equal to the modulusLength member of normalizedAlgorithm
    //    and RSA public exponent equal to the publicExponent member of normalizedAlgorithm.
    // 3. If performing the operation results in an error, then throw an OperationError.
    auto const& normalized_algorithm = static_cast<RsaHashedKeyGenParams const&>(params);
    auto maybe_key_pair = ::Crypto::PK::RSA::generate_key_pair(normalized_algorithm.modulus_length, normalized_algorithm.public_exponent);
    if (maybe_key_pair.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed generating RSA key pair"_string);

    auto key_pair = maybe_key_pair.release_value();

    // 4. Let algorithm be a new RsaHashedKeyAlgorithm object.
    auto algorithm = RsaHashedKeyAlgorithm::create(m_realm);

    // 5. Set the name attribute of algorithm to "RSA-OAEP".
    algorithm->set_name("RSA-OAEP"_string);

    // 6. Set the modulusLength attribute of algorithm to equal the modulusLength member of normalizedAlgorithm.
    algorithm->set_modulus_length(normalized_algorithm.modulus_length);

    // 7. Set the publicExponent attribute of algorithm to equal the publicExponent member of normalizedAlgorithm.
    TRY(algorithm->set_public_exponent(normalized_algorithm.public_exponent));

    // 8. Set the hash attribute of algorithm to equal the hash member of normalizedAlgorithm.
    algorithm->set_hash(normalized_algorithm.hash);

    // 9. Let publicKey be a new CryptoKey representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { key_pair.public_key });

    // 10. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(Bindings::KeyType::Public);

    // 11. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(algorithm);

    // 12. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 13. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "encrypt", "wrapKey" ].
    public_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Encrypt, Bindings::KeyUsage::Wrapkey } }));

    // 14. Let privateKey be a new CryptoKey representing the private key of the generated key pair.
    auto private_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { key_pair.private_key });

    // 15. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(Bindings::KeyType::Private);

    // 16. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(algorithm);

    // 17. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 18. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "decrypt", "unwrapKey" ].
    private_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Decrypt, Bindings::KeyUsage::Unwrapkey } }));

    // 19. Let result be a new CryptoKeyPair dictionary.
    // 20. Set the publicKey attribute of result to be publicKey.
    // 21. Set the privateKey attribute of result to be privateKey.
    // 22. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
    return Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>> { CryptoKeyPair::create(m_realm, public_key, private_key) };
}

// https://w3c.github.io/webcrypto/#rsa-oaep-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> RSAOAEP::import_key(Web::Crypto::AlgorithmParams const& params, Bindings::KeyFormat key_format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& usages)
{
    auto& realm = *m_realm;

    // 1. Let keyData be the key data to be imported.

    GC::Ptr<CryptoKey> key = nullptr;
    auto const& normalized_algorithm = static_cast<RsaHashedImportParams const&>(params);

    // 2. -> If format is "spki":
    if (key_format == Bindings::KeyFormat::Spki) {
        // 1. If usages contains an entry which is not "encrypt" or "wrapKey", then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Encrypt && usage != Bindings::KeyUsage::Wrapkey) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        VERIFY(key_data.has<ByteBuffer>());

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
        //    is not equal to the rsaEncryption object identifier defined in [RFC3447], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::rsa_encryption_oid)
            return WebIDL::DataError::create(m_realm, "Algorithm object identifier is not the rsaEncryption object identifier"_string);

        // 5. Let publicKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the subjectPublicKeyInfo field of spki, structure as the RSAPublicKey structure
        //    specified in Section A.1.1 of [RFC3447], and exactData set to true.
        // NOTE: We already did this in parse_a_subject_public_key_info
        auto& public_key = spki.rsa;

        // 6. If an error occurred while parsing, or it can be determined that publicKey is not
        //    a valid public key according to [RFC3447], then throw a DataError.
        // FIXME: Validate the public key

        // 7. Let key be a new CryptoKey that represents the RSA public key identified by publicKey.
        key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

        // 8. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);
    }

    // -> If format is "pkcs8":
    else if (key_format == Bindings::KeyFormat::Pkcs8) {
        // 1. If usages contains an entry which is not "decrypt" or "unwrapKey", then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Decrypt && usage != Bindings::KeyUsage::Unwrapkey) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        VERIFY(key_data.has<ByteBuffer>());

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo
        //    is not equal to the rsaEncryption object identifier defined in [RFC3447], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::rsa_encryption_oid)
            return WebIDL::DataError::create(m_realm, "Algorithm object identifier is not the rsaEncryption object identifier"_string);

        // 5. Let rsaPrivateKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the privateKey field of privateKeyInfo, structure as the RSAPrivateKey structure
        //    specified in Section A.1.2 of [RFC3447], and exactData set to true.
        // NOTE: We already did this in parse_a_private_key_info
        auto& rsa_private_key = private_key_info.rsa;

        // 6. If an error occurred while parsing, or if rsaPrivateKey is not
        //    a valid RSA private key according to [RFC3447], then throw a DataError.
        // FIXME: Validate the private key

        // 7. Let key be a new CryptoKey that represents the RSA private key identified by rsaPrivateKey.
        key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { rsa_private_key });

        // 8. Set the [[type]] internal slot of key to "private"
        key->set_type(Bindings::KeyType::Private);
    }

    // -> If format is "jwk":
    else if (key_format == Bindings::KeyFormat::Jwk) {
        // 1. -> If keyData is a JsonWebKey dictionary:
        //         Let jwk equal keyData.
        //    -> Otherwise:
        //         Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "keyData is not a JsonWebKey dictionary"_string);
        auto& jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the d field of jwk is present and usages contains an entry which is not "decrypt" or "unwrapKey", then throw a SyntaxError.
        if (jwk.d.has_value()) {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Decrypt && usage != Bindings::KeyUsage::Unwrapkey) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", Bindings::idl_enum_to_string(usage))));
                }
            }
        }

        // 3. If the d field of jwk is not present and usages contains an entry which is not "encrypt" or "wrapKey", then throw a SyntaxError.
        if (!jwk.d.has_value()) {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Encrypt && usage != Bindings::KeyUsage::Wrapkey) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", Bindings::idl_enum_to_string(usage))));
                }
            }
        }

        // 4. If the kty field of jwk is not a case-sensitive string match to "RSA", then throw a DataError.
        if (jwk.kty != "RSA"_string)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 5. If usages is non-empty and the use field of jwk is present and is not a case-sensitive string match to "enc", then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && *jwk.use != "enc"_string)
            return WebIDL::DataError::create(m_realm, "Invalid use field"_string);

        // 6. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
        //    or does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(realm, jwk, usages));

        // 7. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !*jwk.ext && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid ext field"_string);

        Optional<String> hash = {};
        // 8. -> If the alg field of jwk is not present:
        if (!jwk.alg.has_value()) {
            //     Let hash be undefined.
        }
        //    ->  If the alg field of jwk is equal to "RSA-OAEP":
        else if (jwk.alg == "RSA-OAEP"sv) {
            //     Let hash be the string "SHA-1".
            hash = "SHA-1"_string;
        }
        //    -> If the alg field of jwk is equal to "RSA-OAEP-256":
        else if (jwk.alg == "RSA-OAEP-256"sv) {
            //     Let hash be the string "SHA-256".
            hash = "SHA-256"_string;
        }
        //    -> If the alg field of jwk is equal to "RSA-OAEP-384":
        else if (jwk.alg == "RSA-OAEP-384"sv) {
            //     Let hash be the string "SHA-384".
            hash = "SHA-384"_string;
        }
        //    -> If the alg field of jwk is equal to "RSA-OAEP-512":
        else if (jwk.alg == "RSA-OAEP-512"sv) {
            //     Let hash be the string "SHA-512".
            hash = "SHA-512"_string;
        }
        //    -> Otherwise:
        else {
            // FIXME: Support 'other applicable specifications'
            // 1. Perform any key import steps defined by other applicable specifications, passing format, jwk and obtaining hash.
            // 2. If an error occurred or there are no applicable specifications, throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid alg field"_string);
        }

        // 9.  If hash is not undefined:
        if (hash.has_value()) {
            // 1. Let normalizedHash be the result of normalize an algorithm with alg set to hash and op set to digest.
            auto normalized_hash = TRY(normalize_an_algorithm(m_realm, AlgorithmIdentifier { *hash }, "digest"_string));

            // 2. If normalizedHash is not equal to the hash member of normalizedAlgorithm, throw a DataError.
            if (normalized_hash.parameter->name != TRY(normalized_algorithm.hash.name(realm.vm())))
                return WebIDL::DataError::create(m_realm, "Invalid hash"_string);
        }

        // 10. -> If the d field of jwk is present:
        if (jwk.d.has_value()) {
            // 1. If jwk does not meet the requirements of Section 6.3.2 of JSON Web Algorithms [JWA], then throw a DataError.
            bool meets_requirements = jwk.e.has_value() && jwk.n.has_value() && jwk.d.has_value();
            if (jwk.p.has_value() || jwk.q.has_value() || jwk.dp.has_value() || jwk.dq.has_value() || jwk.qi.has_value())
                meets_requirements |= jwk.p.has_value() && jwk.q.has_value() && jwk.dp.has_value() && jwk.dq.has_value() && jwk.qi.has_value();

            if (jwk.oth.has_value()) {
                // FIXME: We don't support > 2 primes in RSA keys
                meets_requirements = false;
            }

            if (!meets_requirements)
                return WebIDL::DataError::create(m_realm, "Invalid JWK private key"_string);

            // 2. Let privateKey represent the RSA private key identified by interpreting jwk according to Section 6.3.2 of JSON Web Algorithms [JWA].
            auto private_key = TRY(parse_jwk_rsa_private_key(realm, jwk));

            // 3. If privateKey can be determined to not be a valid RSA private key according to [RFC3447], then throw a DataError.
            // FIXME: Validate the private key

            // 4. Let key be a new CryptoKey representing privateKey.
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key });

            // 5. Set the [[type]] internal slot of key to "private"
            key->set_type(Bindings::KeyType::Private);
        }

        //     -> Otherwise:
        else {
            // 1. If jwk does not meet the requirements of Section 6.3.1 of JSON Web Algorithms [JWA], then throw a DataError.
            if (!jwk.e.has_value() || !jwk.n.has_value())
                return WebIDL::DataError::create(m_realm, "Invalid JWK public key"_string);

            // 2. Let publicKey represent the RSA public key identified by interpreting jwk according to Section 6.3.1 of JSON Web Algorithms [JWA].
            auto public_key = TRY(parse_jwk_rsa_public_key(realm, jwk));

            // 3. If publicKey can be determined to not be a valid RSA public key according to [RFC3447], then throw a DataError.
            // FIXME: Validate the public key

            // 4. Let key be a new CryptoKey representing publicKey.
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

            // 5. Set the [[type]] internal slot of key to "public"
            key->set_type(Bindings::KeyType::Public);
        }
    }

    // -> Otherwise: throw a NotSupportedError.
    else {
        return WebIDL::NotSupportedError::create(m_realm, "Unsupported key format"_string);
    }

    // 3. Let algorithm be a new RsaHashedKeyAlgorithm.
    auto algorithm = RsaHashedKeyAlgorithm::create(m_realm);

    // 4. Set the name attribute of algorithm to "RSA-OAEP"
    algorithm->set_name("RSA-OAEP"_string);

    // 5. Set the modulusLength attribute of algorithm to the length, in bits, of the RSA public modulus.
    // 6. Set the publicExponent attribute of algorithm to the BigInteger representation of the RSA public exponent.
    TRY(key->handle().visit(
        [&](::Crypto::PK::RSAPublicKey<> const& public_key) -> WebIDL::ExceptionOr<void> {
            algorithm->set_modulus_length(public_key.modulus().trimmed_byte_length() * 8);
            TRY(algorithm->set_public_exponent(public_key.public_exponent()));
            return {};
        },
        [&](::Crypto::PK::RSAPrivateKey<> const& private_key) -> WebIDL::ExceptionOr<void> {
            algorithm->set_modulus_length(private_key.modulus().trimmed_byte_length() * 8);
            TRY(algorithm->set_public_exponent(private_key.public_exponent()));
            return {};
        },
        [](auto) -> WebIDL::ExceptionOr<void> { VERIFY_NOT_REACHED(); }));

    // 7. Set the hash attribute of algorithm to the hash member of normalizedAlgorithm.
    algorithm->set_hash(normalized_algorithm.hash);

    // 8. Set the [[algorithm]] internal slot of key to algorithm
    key->set_algorithm(algorithm);

    // 9. Return key.
    return GC::Ref { *key };
}

// https://w3c.github.io/webcrypto/#rsa-oaep-operations
WebIDL::ExceptionOr<GC::Ref<JS::Object>> RSAOAEP::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    auto& realm = *m_realm;
    auto& vm = realm.vm();

    // 1. Let key be the key to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    GC::Ptr<JS::Object> result = nullptr;

    // 3. If format is "spki"
    if (format == Bindings::KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(realm, "Key is not public"_string);

        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        // - Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //   - Set the algorithm field to the OID rsaEncryption defined in [RFC3447].
        //   - Set the params field to the ASN.1 type NULL.
        // - Set the subjectPublicKey field to the result of DER-encoding an RSAPublicKey ASN.1 type, as defined in [RFC3447], Appendix A.1.1,
        //   that represents the RSA public key represented by the [[handle]] internal slot of key
        auto maybe_data = handle.visit(
            [&](::Crypto::PK::RSAPublicKey<> const& public_key) -> ErrorOr<ByteBuffer> {
                return TRY(::Crypto::PK::wrap_in_subject_public_key_info(public_key, ::Crypto::ASN1::rsa_encryption_oid, nullptr));
            },
            [](auto) -> ErrorOr<ByteBuffer> {
                VERIFY_NOT_REACHED();
            });
        // FIXME: clang-format butchers the visit if we do the TRY inline
        auto data = TRY_OR_THROW_OOM(vm, maybe_data);

        // 3. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(realm, data);
    }

    // If format is "pkcs8"
    else if (format == Bindings::KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Private)
            return WebIDL::InvalidAccessError::create(realm, "Key is not private"_string);

        // 2. Let data be the result of encoding a privateKeyInfo structure with the following properties:
        // - Set the version field to 0.
        // - Set the privateKeyAlgorithm field to an PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        // - - Set the algorithm field to the OID rsaEncryption defined in [RFC3447].
        // - - Set the params field to the ASN.1 type NULL.
        // - Set the privateKey field to the result of DER-encoding an RSAPrivateKey ASN.1 type, as defined in [RFC3447], Appendix A.1.2,
        // that represents the RSA private key represented by the [[handle]] internal slot of key
        auto maybe_data = handle.visit(
            [&](::Crypto::PK::RSAPrivateKey<> const& private_key) -> ErrorOr<ByteBuffer> {
                return TRY(::Crypto::PK::wrap_in_private_key_info(private_key, ::Crypto::ASN1::rsa_encryption_oid, nullptr));
            },
            [](auto) -> ErrorOr<ByteBuffer> {
                VERIFY_NOT_REACHED();
            });

        // FIXME: clang-format butchers the visit if we do the TRY inline
        auto data = TRY_OR_THROW_OOM(vm, maybe_data);

        // 3. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(realm, data);
    }

    // If format is "jwk"
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to the string "RSA".
        jwk.kty = "RSA"_string;

        // 4. Let hash be the name attribute of the hash attribute of the [[algorithm]] internal slot of key.
        auto hash = TRY(as<RsaHashedKeyAlgorithm>(*key->algorithm()).hash().name(vm));

        // 4. If hash is "SHA-1":
        //      - Set the alg attribute of jwk to the string "RSA-OAEP".
        if (hash == "SHA-1"sv) {
            jwk.alg = "RSA-OAEP"_string;
        }
        //    If hash is "SHA-256":
        //      - Set the alg attribute of jwk to the string "RSA-OAEP-256".
        else if (hash == "SHA-256"sv) {
            jwk.alg = "RSA-OAEP-256"_string;
        }
        //    If hash is "SHA-384":
        //      - Set the alg attribute of jwk to the string "RSA-OAEP-384".
        else if (hash == "SHA-384"sv) {
            jwk.alg = "RSA-OAEP-384"_string;
        }
        //    If hash is "SHA-512":
        //      - Set the alg attribute of jwk to the string "RSA-OAEP-512".
        else if (hash == "SHA-512"sv) {
            jwk.alg = "RSA-OAEP-512"_string;
        } else {
            // FIXME: Support 'other applicable specifications'
            // - Perform any key export steps defined by other applicable specifications,
            //   passing format and the hash attribute of the [[algorithm]] internal slot of key and obtaining alg.
            // - Set the alg attribute of jwk to alg.
            return WebIDL::NotSupportedError::create(realm, TRY_OR_THROW_OOM(vm, String::formatted("Unsupported hash algorithm '{}'", hash)));
        }

        // 10. Set the attributes n and e of jwk according to the corresponding definitions in JSON Web Algorithms [JWA], Section 6.3.1.
        auto maybe_error = handle.visit(
            [&](::Crypto::PK::RSAPublicKey<> const& public_key) -> ErrorOr<void> {
                jwk.n = TRY(base64_url_uint_encode(public_key.modulus()));
                jwk.e = TRY(base64_url_uint_encode(public_key.public_exponent()));
                return {};
            },
            [&](::Crypto::PK::RSAPrivateKey<> const& private_key) -> ErrorOr<void> {
                jwk.n = TRY(base64_url_uint_encode(private_key.modulus()));
                jwk.e = TRY(base64_url_uint_encode(private_key.public_exponent()));

                // 11. If the [[type]] internal slot of key is "private":
                //    1. Set the attributes named d, p, q, dp, dq, and qi of jwk according to the corresponding definitions in JSON Web Algorithms [JWA], Section 6.3.2.
                jwk.d = TRY(base64_url_uint_encode(private_key.private_exponent()));
                jwk.p = TRY(base64_url_uint_encode(private_key.prime1()));
                jwk.q = TRY(base64_url_uint_encode(private_key.prime2()));
                jwk.dp = TRY(base64_url_uint_encode(private_key.exponent1()));
                jwk.dq = TRY(base64_url_uint_encode(private_key.exponent2()));
                jwk.qi = TRY(base64_url_uint_encode(private_key.coefficient()));

                // 12. If the underlying RSA private key represented by the [[handle]] internal slot of key is represented by more than two primes,
                //     set the attribute named oth of jwk according to the corresponding definition in JSON Web Algorithms [JWA], Section 6.3.2.7
                // FIXME: We don't support more than 2 primes on RSA keys
                return {};
            },
            [](auto) -> ErrorOr<void> {
                VERIFY_NOT_REACHED();
            });
        // FIXME: clang-format butchers the visit if we do the TRY inline
        TRY_OR_THROW_OOM(vm, maybe_error);

        // 13. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages()) {
            jwk.key_ops->append(Bindings::idl_enum_to_string(usage));
        }

        // 14. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 15. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        result = TRY(jwk.to_object(realm));
    }

    // Otherwise throw a NotSupportedError.
    else {
        return WebIDL::NotSupportedError::create(realm, TRY_OR_THROW_OOM(vm, String::formatted("Exporting to format {} is not supported", Bindings::idl_enum_to_string(format))));
    }

    // 8. Return result
    return GC::Ref { *result };
}

// https://w3c.github.io/webcrypto/#rsa-pss-operations
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> RSAPSS::generate_key(AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains a value which is not one of "sign" or "verify", then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Sign && usage != Bindings::KeyUsage::Verify) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    // 2. Generate an RSA key pair, as defined in [RFC3447], with RSA modulus length equal to the modulusLength member of normalizedAlgorithm
    //    and RSA public exponent equal to the publicExponent member of normalizedAlgorithm.
    // 3. If performing the operation results in an error, then throw an OperationError.
    auto const& normalized_algorithm = static_cast<RsaHashedKeyGenParams const&>(params);
    auto maybe_key_pair = ::Crypto::PK::RSA::generate_key_pair(normalized_algorithm.modulus_length, normalized_algorithm.public_exponent);
    if (maybe_key_pair.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to generate RSA key pair"_string);

    auto key_pair = maybe_key_pair.release_value();

    // 4. Let algorithm be a new RsaHashedKeyAlgorithm object.
    auto algorithm = RsaHashedKeyAlgorithm::create(m_realm);

    // 5. Set the name attribute of algorithm to "RSA-PSS".
    algorithm->set_name("RSA-PSS"_string);

    // 6. Set the modulusLength attribute of algorithm to equal the modulusLength member of normalizedAlgorithm.
    algorithm->set_modulus_length(normalized_algorithm.modulus_length);

    // 7. Set the publicExponent attribute of algorithm to equal the publicExponent member of normalizedAlgorithm.
    TRY(algorithm->set_public_exponent(normalized_algorithm.public_exponent));

    // 8. Set the hash attribute of algorithm to equal the hash member of normalizedAlgorithm.
    algorithm->set_hash(normalized_algorithm.hash);

    // 9. Let publicKey be a new CryptoKey representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { key_pair.public_key });

    // 10. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(Bindings::KeyType::Public);

    // 11. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(algorithm);

    // 12. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 13. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
    public_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Verify } }));

    // 14. Let privateKey be a new CryptoKey representing the private key of the generated key pair.
    auto private_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { key_pair.private_key });

    // 15. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(Bindings::KeyType::Private);

    // 16. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(algorithm);

    // 17. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 18. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
    private_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Sign } }));

    // 19. Let result be a new CryptoKeyPair dictionary.
    // 20. Set the publicKey attribute of result to be publicKey.
    // 21. Set the privateKey attribute of result to be privateKey.
    // 22. Return result.
    return Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>> { CryptoKeyPair::create(m_realm, public_key, private_key) };
}

// https://w3c.github.io/webcrypto/#rsa-pss-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> RSAPSS::sign(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& message)
{
    auto& realm = *m_realm;
    auto& vm = realm.vm();

    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Private)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a private key"_string);

    auto const& private_key = key->handle().get<::Crypto::PK::RSAPrivateKey<>>();
    auto pss_params = static_cast<RsaPssParams const&>(params);
    auto hash = TRY(as<RsaHashedKeyAlgorithm>(*key->algorithm()).hash().name(vm));

    // 3. Perform the signature generation operation defined in Section 8.1 of [RFC3447] with the key represented by the [[handle]] internal slot
    //    of key as the signer's private key, K, and the contents of message as the message to be signed, M, and using the hash function specified
    //    by the hash attribute of the [[algorithm]] internal slot of key as the Hash option, MGF1 (defined in Section B.2.1 of [RFC3447])
    //    as the MGF option and the saltLength member of normalizedAlgorithm as the salt length option for the EMSA-PSS-ENCODE operation.
    Optional<::Crypto::Hash::HashKind> hash_kind = {};
    if (hash == "SHA-1")
        hash_kind = ::Crypto::Hash::HashKind::SHA1;
    else if (hash == "SHA-256")
        hash_kind = ::Crypto::Hash::HashKind::SHA256;
    else if (hash == "SHA-384")
        hash_kind = ::Crypto::Hash::HashKind::SHA384;
    else if (hash == "SHA-512")
        hash_kind = ::Crypto::Hash::HashKind::SHA512;

    // 4. If performing the operation results in an error, then throw an OperationError.
    if (!hash_kind.has_value()) {
        auto error_message = MUST(String::formatted("Invalid hash function '{}'", hash));
        return WebIDL::OperationError::create(realm, error_message);
    }

    // 5. Let signature be the signature, S, that results from performing the operation.
    auto rsa = ::Crypto::PK::RSA_PSS_EMSA { *hash_kind, private_key };
    rsa.set_salt_length(pss_params.salt_length);

    auto maybe_signature = rsa.sign(message);
    if (maybe_signature.is_error())
        return WebIDL::OperationError::create(realm, "Failed to sign message"_string);

    // 6. Return signature.
    return JS::ArrayBuffer::create(realm, maybe_signature.release_value());
}

// https://w3c.github.io/webcrypto/#rsa-pss-operations
WebIDL::ExceptionOr<JS::Value> RSAPSS::verify(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& signature, ByteBuffer const& message)
{
    auto& realm = *m_realm;
    auto& vm = realm.vm();

    // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Public)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a public key"_string);

    auto const& public_key = key->handle().get<::Crypto::PK::RSAPublicKey<>>();
    auto pss_params = static_cast<RsaPssParams const&>(params);
    auto hash = TRY(as<RsaHashedKeyAlgorithm>(*key->algorithm()).hash().name(vm));

    // 2. Perform the signature verification operation defined in Section 8.1 of [RFC3447] with the key represented by the [[handle]] internal slot
    //    of key as the signer's RSA public key and the contents of message as M and the contents of signature as S and using the hash function specified
    //    by the hash attribute of the [[algorithm]] internal slot of key as the Hash option, MGF1 (defined in Section B.2.1 of [RFC3447])
    //    as the MGF option and the saltLength member of normalizedAlgorithm as the salt length option for the EMSA-PSS-VERIFY operation.
    Optional<::Crypto::Hash::HashKind> hash_kind = {};
    if (hash == "SHA-1")
        hash_kind = ::Crypto::Hash::HashKind::SHA1;
    else if (hash == "SHA-256")
        hash_kind = ::Crypto::Hash::HashKind::SHA256;
    else if (hash == "SHA-384")
        hash_kind = ::Crypto::Hash::HashKind::SHA384;
    else if (hash == "SHA-512")
        hash_kind = ::Crypto::Hash::HashKind::SHA512;

    if (!hash_kind.has_value()) {
        auto error_message = MUST(String::formatted("Invalid hash function '{}'", hash));
        return WebIDL::OperationError::create(realm, error_message);
    }

    // 3. Let result be a boolean with the value true if the result of the operation was "valid signature" and the value false otherwise.
    auto rsa = ::Crypto::PK::RSA_PSS_EMSA { *hash_kind, public_key };
    rsa.set_salt_length(pss_params.salt_length);

    auto maybe_verification = rsa.verify(message, signature);
    if (maybe_verification.is_error())
        return WebIDL::OperationError::create(realm, "Failed to verify message"_string);

    return JS::Value { maybe_verification.release_value() };
}

// https://w3c.github.io/webcrypto/#rsa-pss-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> RSAPSS::import_key(AlgorithmParams const& params, Bindings::KeyFormat key_format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& usages)
{
    auto& realm = *m_realm;

    // 1. Let keyData be the key data to be imported.

    GC::Ptr<CryptoKey> key = nullptr;
    auto const& normalized_algorithm = static_cast<RsaHashedImportParams const&>(params);

    // 2. -> If format is "spki":
    if (key_format == Bindings::KeyFormat::Spki) {
        // 1. If usages contains an entry which is not "verify" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Verify) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        VERIFY(key_data.has<ByteBuffer>());

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
        //    is not equal to the rsaEncryption object identifier defined in [RFC3447], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::rsa_encryption_oid)
            return WebIDL::DataError::create(m_realm, "Algorithm object identifier is not the rsaEncryption object identifier"_string);

        // 5. Let publicKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the subjectPublicKeyInfo field of spki, structure as the RSAPublicKey structure
        //    specified in Section A.1.1 of [RFC3447], and exactData set to true.
        // NOTE: We already did this in parse_a_subject_public_key_info
        auto& public_key = spki.rsa;

        // 6. If an error occurred while parsing, or it can be determined that publicKey is not
        //    a valid public key according to [RFC3447], then throw a DataError.
        // FIXME: Validate the public key

        // 7. Let key be a new CryptoKey that represents the RSA public key identified by publicKey.
        key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

        // 8. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);
    }

    // -> If format is "pkcs8":
    else if (key_format == Bindings::KeyFormat::Pkcs8) {
        // 1. If usages contains an entry which is not "sign" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Sign) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        VERIFY(key_data.has<ByteBuffer>());

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo
        //    is not equal to the rsaEncryption object identifier defined in [RFC3447], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::rsa_encryption_oid)
            return WebIDL::DataError::create(m_realm, "Algorithm object identifier is not the rsaEncryption object identifier"_string);

        // 5. Let rsaPrivateKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the privateKey field of privateKeyInfo, structure as the RSAPrivateKey structure
        //    specified in Section A.1.2 of [RFC3447], and exactData set to true.
        // NOTE: We already did this in parse_a_private_key_info
        auto& rsa_private_key = private_key_info.rsa;

        // 6. If an error occurred while parsing, or if rsaPrivateKey is not
        //    a valid RSA private key according to [RFC3447], then throw a DataError.
        // FIXME: Validate the private key

        // 7. Let key be a new CryptoKey that represents the RSA private key identified by rsaPrivateKey.
        key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { rsa_private_key });

        // 8. Set the [[type]] internal slot of key to "private"
        key->set_type(Bindings::KeyType::Private);
    }

    // -> If format is "jwk":
    else if (key_format == Bindings::KeyFormat::Jwk) {
        // 1. -> If keyData is a JsonWebKey dictionary:
        //         Let jwk equal keyData.
        //    -> Otherwise:
        //         Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "keyData is not a JsonWebKey dictionary"_string);
        auto& jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the d field of jwk is present and usages contains an entry which is not "sign", or,
        //    if the d field of jwk is not present and usages contains an entry which is not "verify"
        //    then throw a SyntaxError.
        if (jwk.d.has_value()) {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Sign) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", Bindings::idl_enum_to_string(usage))));
                }
            }
        } else {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Verify) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", Bindings::idl_enum_to_string(usage))));
                }
            }
        }

        // 3. If the kty field of jwk is not a case-sensitive string match to "RSA", then throw a DataError.
        if (jwk.kty != "RSA"_string)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 4. If usages is non-empty and the use field of jwk is present and is not a case-sensitive string match to "sig", then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && *jwk.use != "sig"_string)
            return WebIDL::DataError::create(m_realm, "Invalid use field"_string);

        // 5. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
        //    or does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(realm, jwk, usages));

        // 6. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !*jwk.ext && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid ext field"_string);

        Optional<String> hash = {};
        // 7. -> If the alg field of jwk is not present:
        if (!jwk.alg.has_value()) {
            //     Let hash be undefined.
        }
        //    ->  If the alg field of jwk is equal to "PS1":
        else if (jwk.alg == "PS1"sv) {
            //     Let hash be the string "SHA-1".
            hash = "SHA-1"_string;
        }
        //    -> If the alg field of jwk is equal to "PS256":
        else if (jwk.alg == "PS256"sv) {
            //     Let hash be the string "SHA-256".
            hash = "SHA-256"_string;
        }
        //    -> If the alg field of jwk is equal to "PS384":
        else if (jwk.alg == "PS384"sv) {
            //     Let hash be the string "SHA-384".
            hash = "SHA-384"_string;
        }
        //    -> If the alg field of jwk is equal to "PS512":
        else if (jwk.alg == "PS512"sv) {
            //     Let hash be the string "SHA-512".
            hash = "SHA-512"_string;
        }
        //    -> Otherwise:
        else {
            // FIXME: Support 'other applicable specifications'
            // 1. Perform any key import steps defined by other applicable specifications, passing format, jwk and obtaining hash.
            // 2. If an error occurred or there are no applicable specifications, throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid alg field"_string);
        }

        // 8. If hash is not undefined:
        if (hash.has_value()) {
            // 1. Let normalizedHash be the result of normalize an algorithm with alg set to hash and op set to digest.
            auto normalized_hash = TRY(normalize_an_algorithm(m_realm, AlgorithmIdentifier { *hash }, "digest"_string));

            // 2. If normalizedHash is not equal to the hash member of normalizedAlgorithm, throw a DataError.
            if (normalized_hash.parameter->name != TRY(normalized_algorithm.hash.name(realm.vm())))
                return WebIDL::DataError::create(m_realm, "Invalid hash"_string);
        }

        // 9. -> If the d field of jwk is present:
        if (jwk.d.has_value()) {
            // 1. If jwk does not meet the requirements of Section 6.3.2 of JSON Web Algorithms [JWA], then throw a DataError.
            bool meets_requirements = jwk.e.has_value() && jwk.n.has_value() && jwk.d.has_value();
            if (jwk.p.has_value() || jwk.q.has_value() || jwk.dp.has_value() || jwk.dq.has_value() || jwk.qi.has_value())
                meets_requirements |= jwk.p.has_value() && jwk.q.has_value() && jwk.dp.has_value() && jwk.dq.has_value() && jwk.qi.has_value();

            if (jwk.oth.has_value()) {
                // FIXME: We don't support > 2 primes in RSA keys
                meets_requirements = false;
            }

            if (!meets_requirements)
                return WebIDL::DataError::create(m_realm, "Invalid JWK private key"_string);

            // FIXME: Spec error, it should say 'the RSA private key identified by interpreting jwk according to section 6.3.2'
            // 2. Let privateKey represent the RSA public key identified by interpreting jwk according to Section 6.3.1 of JSON Web Algorithms [JWA].
            auto private_key = TRY(parse_jwk_rsa_private_key(realm, jwk));

            // FIXME: Spec error, it should say 'not to be a valid RSA private key'
            // 3. If privateKey can be determined to not be a valid RSA public key according to [RFC3447], then throw a DataError.
            // FIXME: Validate the private key

            // 4. Let key be a new CryptoKey representing privateKey.
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key });

            // 5. Set the [[type]] internal slot of key to "private"
            key->set_type(Bindings::KeyType::Private);
        }

        //     -> Otherwise:
        else {
            // 1. If jwk does not meet the requirements of Section 6.3.1 of JSON Web Algorithms [JWA], then throw a DataError.
            if (!jwk.e.has_value() || !jwk.n.has_value())
                return WebIDL::DataError::create(m_realm, "Invalid JWK public key"_string);

            // 2. Let publicKey represent the RSA public key identified by interpreting jwk according to Section 6.3.1 of JSON Web Algorithms [JWA].
            auto public_key = TRY(parse_jwk_rsa_public_key(realm, jwk));

            // 3. If publicKey can be determined to not be a valid RSA public key according to [RFC3447], then throw a DataError.
            // FIXME: Validate the public key

            // 4. Let key be a new CryptoKey representing publicKey.
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

            // 5. Set the [[type]] internal slot of key to "public"
            key->set_type(Bindings::KeyType::Public);
        }
    }

    // -> Otherwise: throw a NotSupportedError.
    else {
        return WebIDL::NotSupportedError::create(m_realm, "Unsupported key format"_string);
    }

    // 3. Let algorithm be a new RsaHashedKeyAlgorithm.
    auto algorithm = RsaHashedKeyAlgorithm::create(m_realm);

    // 4. Set the name attribute of algorithm to "RSA-PSS"
    algorithm->set_name("RSA-PSS"_string);

    // 5. Set the modulusLength attribute of algorithm to the length, in bits, of the RSA public modulus.
    // 6. Set the publicExponent attribute of algorithm to the BigInteger representation of the RSA public exponent.
    TRY(key->handle().visit(
        [&](::Crypto::PK::RSAPublicKey<> const& public_key) -> WebIDL::ExceptionOr<void> {
            algorithm->set_modulus_length(public_key.modulus().trimmed_byte_length() * 8);
            TRY(algorithm->set_public_exponent(public_key.public_exponent()));
            return {};
        },
        [&](::Crypto::PK::RSAPrivateKey<> const& private_key) -> WebIDL::ExceptionOr<void> {
            algorithm->set_modulus_length(private_key.modulus().trimmed_byte_length() * 8);
            TRY(algorithm->set_public_exponent(private_key.public_exponent()));
            return {};
        },
        [](auto) -> WebIDL::ExceptionOr<void> { VERIFY_NOT_REACHED(); }));

    // 7. Set the hash attribute of algorithm to the hash member of normalizedAlgorithm.
    algorithm->set_hash(normalized_algorithm.hash);

    // 8. Set the [[algorithm]] internal slot of key to algorithm
    key->set_algorithm(algorithm);

    // 9. Return key.
    return GC::Ref { *key };
}

// https://w3c.github.io/webcrypto/#rsa-pss-operations
WebIDL::ExceptionOr<GC::Ref<JS::Object>> RSAPSS::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    auto& realm = *m_realm;
    auto& vm = realm.vm();

    // 1. Let key be the key to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    GC::Ptr<JS::Object> result = nullptr;

    // 3. If format is "spki"
    if (format == Bindings::KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(realm, "Key is not public"_string);

        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        // - Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //   - Set the algorithm field to the OID rsaEncryption defined in [RFC3447].
        //   - Set the params field to the ASN.1 type NULL.
        // - Set the subjectPublicKey field to the result of DER-encoding an RSAPublicKey ASN.1 type, as defined in [RFC3447], Appendix A.1.1,
        //   that represents the RSA public key represented by the [[handle]] internal slot of key
        auto maybe_data = handle.visit(
            [&](::Crypto::PK::RSAPublicKey<> const& public_key) -> ErrorOr<ByteBuffer> {
                return TRY(::Crypto::PK::wrap_in_subject_public_key_info(public_key, ::Crypto::ASN1::rsa_encryption_oid, nullptr));
            },
            [](auto) -> ErrorOr<ByteBuffer> {
                VERIFY_NOT_REACHED();
            });
        // FIXME: clang-format butchers the visit if we do the TRY inline
        auto data = TRY_OR_THROW_OOM(vm, maybe_data);

        // 3. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(realm, data);
    }

    // If format is "pkcs8"
    else if (format == Bindings::KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Private)
            return WebIDL::InvalidAccessError::create(realm, "Key is not private"_string);

        // 2. Let data be the result of encoding a privateKeyInfo structure with the following properties:
        // - Set the version field to 0.
        // - Set the privateKeyAlgorithm field to an PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        // - - Set the algorithm field to the OID rsaEncryption defined in [RFC3447].
        // - - Set the params field to the ASN.1 type NULL.
        // - Set the privateKey field to the result of DER-encoding an RSAPrivateKey ASN.1 type, as defined in [RFC3447], Appendix A.1.2,
        // that represents the RSA private key represented by the [[handle]] internal slot of key
        auto maybe_data = handle.visit(
            [&](::Crypto::PK::RSAPrivateKey<> const& private_key) -> ErrorOr<ByteBuffer> {
                return TRY(::Crypto::PK::wrap_in_private_key_info(private_key, ::Crypto::ASN1::rsa_encryption_oid, nullptr));
            },
            [](auto) -> ErrorOr<ByteBuffer> {
                VERIFY_NOT_REACHED();
            });

        // FIXME: clang-format butchers the visit if we do the TRY inline
        auto data = TRY_OR_THROW_OOM(vm, maybe_data);

        // 3. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(realm, data);
    }

    // If format is "jwk"
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to the string "RSA".
        jwk.kty = "RSA"_string;

        // 3. Let hash be the name attribute of the hash attribute of the [[algorithm]] internal slot of key.
        auto hash = TRY(as<RsaHashedKeyAlgorithm>(*key->algorithm()).hash().name(vm));

        // 4. If hash is "SHA-1":
        //      - Set the alg attribute of jwk to the string "PS1".
        if (hash == "SHA-1"sv) {
            jwk.alg = "PS1"_string;
        }
        //    If hash is "SHA-256":
        //      - Set the alg attribute of jwk to the string "PS256".
        else if (hash == "SHA-256"sv) {
            jwk.alg = "PS256"_string;
        }
        //    If hash is "SHA-384":
        //      - Set the alg attribute of jwk to the string "PS384".
        else if (hash == "SHA-384"sv) {
            jwk.alg = "PS384"_string;
        }
        //    If hash is "SHA-512":
        //      - Set the alg attribute of jwk to the string "PS512".
        else if (hash == "SHA-512"sv) {
            jwk.alg = "PS512"_string;
        } else {
            // FIXME: Support 'other applicable specifications'
            // - Perform any key export steps defined by other applicable specifications,
            //   passing format and the hash attribute of the [[algorithm]] internal slot of key and obtaining alg.
            // - Set the alg attribute of jwk to alg.
            return WebIDL::NotSupportedError::create(realm, TRY_OR_THROW_OOM(vm, String::formatted("Unsupported hash algorithm '{}'", hash)));
        }

        // 5. Set the attributes n and e of jwk according to the corresponding definitions in JSON Web Algorithms [JWA], Section 6.3.1.
        auto maybe_error = handle.visit(
            [&](::Crypto::PK::RSAPublicKey<> const& public_key) -> ErrorOr<void> {
                jwk.n = TRY(base64_url_uint_encode(public_key.modulus()));
                jwk.e = TRY(base64_url_uint_encode(public_key.public_exponent()));
                return {};
            },
            // 6. If the [[type]] internal slot of key is "private":
            [&](::Crypto::PK::RSAPrivateKey<> const& private_key) -> ErrorOr<void> {
                jwk.n = TRY(base64_url_uint_encode(private_key.modulus()));
                jwk.e = TRY(base64_url_uint_encode(private_key.public_exponent()));

                // 1. Set the attributes named d, p, q, dp, dq, and qi of jwk according to the corresponding definitions
                //    in JSON Web Algorithms [JWA], Section 6.3.2.
                jwk.d = TRY(base64_url_uint_encode(private_key.private_exponent()));
                jwk.p = TRY(base64_url_uint_encode(private_key.prime1()));
                jwk.q = TRY(base64_url_uint_encode(private_key.prime2()));
                jwk.dp = TRY(base64_url_uint_encode(private_key.exponent1()));
                jwk.dq = TRY(base64_url_uint_encode(private_key.exponent2()));
                jwk.qi = TRY(base64_url_uint_encode(private_key.coefficient()));

                // 2. If the underlying RSA private key represented by the [[handle]] internal slot of key is represented by more than two primes,
                //    set the attribute named oth of jwk according to the corresponding definition in JSON Web Algorithms [JWA], Section 6.3.2.7
                // FIXME: We don't support more than 2 primes on RSA keys
                return {};
            },
            [](auto) -> ErrorOr<void> {
                VERIFY_NOT_REACHED();
            });
        // FIXME: clang-format butchers the visit if we do the TRY inline
        TRY_OR_THROW_OOM(vm, maybe_error);

        // 7. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages()) {
            jwk.key_ops->append(Bindings::idl_enum_to_string(usage));
        }

        // 8. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 9. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        result = TRY(jwk.to_object(realm));
    }

    // Otherwise throw a NotSupportedError.
    else {
        return WebIDL::NotSupportedError::create(realm, TRY_OR_THROW_OOM(vm, String::formatted("Exporting to format {} is not supported", Bindings::idl_enum_to_string(format))));
    }

    // 8. Return result
    return GC::Ref { *result };
}

// https://w3c.github.io/webcrypto/#rsassa-pkcs1-operations
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> RSASSAPKCS1::generate_key(AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains a value which is not one of "sign" or "verify", then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Sign && usage != Bindings::KeyUsage::Verify) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    // 2. Generate an RSA key pair, as defined in [RFC3447], with RSA modulus length equal to the modulusLength member of normalizedAlgorithm
    //    and RSA public exponent equal to the publicExponent member of normalizedAlgorithm.
    // 3. If performing the operation results in an error, then throw an OperationError.
    auto const& normalized_algorithm = static_cast<RsaHashedKeyGenParams const&>(params);
    auto maybe_key_pair = ::Crypto::PK::RSA::generate_key_pair(normalized_algorithm.modulus_length, normalized_algorithm.public_exponent);
    if (maybe_key_pair.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to generate RSA key pair"_string);

    auto key_pair = maybe_key_pair.release_value();

    // 4. Let algorithm be a new RsaHashedKeyAlgorithm object.
    auto algorithm = RsaHashedKeyAlgorithm::create(m_realm);

    // 5. Set the name attribute of algorithm to "RSASSA-PKCS1-v1_5".
    algorithm->set_name("RSASSA-PKCS1-v1_5"_string);

    // 6. Set the modulusLength attribute of algorithm to equal the modulusLength member of normalizedAlgorithm.
    algorithm->set_modulus_length(normalized_algorithm.modulus_length);

    // 7. Set the publicExponent attribute of algorithm to equal the publicExponent member of normalizedAlgorithm.
    TRY(algorithm->set_public_exponent(normalized_algorithm.public_exponent));

    // 8. Set the hash attribute of algorithm to equal the hash member of normalizedAlgorithm.
    algorithm->set_hash(normalized_algorithm.hash);

    // 9. Let publicKey be a new CryptoKey representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { key_pair.public_key });

    // 10. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(Bindings::KeyType::Public);

    // 11. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(algorithm);

    // 12. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 13. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
    public_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Verify } }));

    // 14. Let privateKey be a new CryptoKey representing the private key of the generated key pair.
    auto private_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { key_pair.private_key });

    // 15. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(Bindings::KeyType::Private);

    // 16. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(algorithm);

    // 17. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 18. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
    private_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Sign } }));

    // 19. Let result be a new CryptoKeyPair dictionary.
    // 20. Set the publicKey attribute of result to be publicKey.
    // 21. Set the privateKey attribute of result to be privateKey.
    // 22. Return result.
    return Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>> { CryptoKeyPair::create(m_realm, public_key, private_key) };
}

// https://w3c.github.io/webcrypto/#rsassa-pkcs1-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> RSASSAPKCS1::sign(AlgorithmParams const&, GC::Ref<CryptoKey> key, ByteBuffer const& message)
{
    auto& realm = *m_realm;
    auto& vm = realm.vm();

    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Private)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a private key"_string);

    auto const& private_key = key->handle().get<::Crypto::PK::RSAPrivateKey<>>();
    auto hash = TRY(as<RsaHashedKeyAlgorithm>(*key->algorithm()).hash().name(vm));

    // 3. Perform the signature generation operation defined in Section 8.2 of [RFC3447] with the key represented by the [[handle]] internal slot
    //    of key as the signer's private key and the contents of message as M and using the hash function specified in the hash attribute
    //    of the [[algorithm]] internal slot of key as the Hash option for the EMSA-PKCS1-v1_5 encoding method.
    Optional<::Crypto::Hash::HashKind> hash_kind = {};
    if (hash == "SHA-1")
        hash_kind = ::Crypto::Hash::HashKind::SHA1;
    else if (hash == "SHA-256")
        hash_kind = ::Crypto::Hash::HashKind::SHA256;
    else if (hash == "SHA-384")
        hash_kind = ::Crypto::Hash::HashKind::SHA384;
    else if (hash == "SHA-512")
        hash_kind = ::Crypto::Hash::HashKind::SHA512;

    // 4. If performing the operation results in an error, then throw an OperationError.
    if (!hash_kind.has_value()) {
        auto error_message = MUST(String::formatted("Invalid hash function '{}'", hash));
        return WebIDL::OperationError::create(realm, error_message);
    }

    // 5. Let signature be the signature, S, that results from performing the operation.
    auto rsa = ::Crypto::PK::RSA_PKCS1_EMSA { *hash_kind, private_key };

    auto maybe_signature = rsa.sign(message);
    if (maybe_signature.is_error())
        return WebIDL::OperationError::create(realm, "Failed to sign message"_string);

    // 6. Return signature.
    return JS::ArrayBuffer::create(realm, maybe_signature.release_value());
}

// https://w3c.github.io/webcrypto/#rsassa-pkcs1-operations
WebIDL::ExceptionOr<JS::Value> RSASSAPKCS1::verify(AlgorithmParams const&, GC::Ref<CryptoKey> key, ByteBuffer const& signature, ByteBuffer const& message)
{
    auto& realm = *m_realm;
    auto& vm = realm.vm();

    // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Public)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a public key"_string);

    auto const& public_key = key->handle().get<::Crypto::PK::RSAPublicKey<>>();
    auto hash = TRY(as<RsaHashedKeyAlgorithm>(*key->algorithm()).hash().name(vm));

    // 2. Perform the signature verification operation defined in Section 8.2 of [RFC3447] with the key represented by the [[handle]] internal slot
    //    of key as the signer's RSA public key and the contents of message as M and the contents of signature as S and using the hash function specified
    //    in the hash attribute of the [[algorithm]] internal slot of key as the Hash option for the EMSA-PKCS1-v1_5 encoding method.
    Optional<::Crypto::Hash::HashKind> hash_kind = {};
    if (hash == "SHA-1")
        hash_kind = ::Crypto::Hash::HashKind::SHA1;
    else if (hash == "SHA-256")
        hash_kind = ::Crypto::Hash::HashKind::SHA256;
    else if (hash == "SHA-384")
        hash_kind = ::Crypto::Hash::HashKind::SHA384;
    else if (hash == "SHA-512")
        hash_kind = ::Crypto::Hash::HashKind::SHA512;

    if (!hash_kind.has_value()) {
        auto error_message = MUST(String::formatted("Invalid hash function '{}'", hash));
        return WebIDL::OperationError::create(realm, error_message);
    }

    // 3. Let result be a boolean with the value true if the result of the operation was "valid signature" and the value false otherwise.
    auto rsa = ::Crypto::PK::RSA_PKCS1_EMSA { *hash_kind, public_key };

    auto maybe_verification = rsa.verify(message, signature);
    if (maybe_verification.is_error())
        return WebIDL::OperationError::create(realm, "Failed to verify message"_string);

    return JS::Value { maybe_verification.release_value() };
}

// https://w3c.github.io/webcrypto/#rsassa-pkcs1-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> RSASSAPKCS1::import_key(AlgorithmParams const& params, Bindings::KeyFormat key_format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& usages)
{
    auto& realm = *m_realm;

    // 1. Let keyData be the key data to be imported.

    GC::Ptr<CryptoKey> key = nullptr;
    auto const& normalized_algorithm = static_cast<RsaHashedImportParams const&>(params);

    // 2. -> If format is "spki":
    if (key_format == Bindings::KeyFormat::Spki) {
        // 1. If usages contains an entry which is not "verify" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Verify) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        VERIFY(key_data.has<ByteBuffer>());

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
        //    is not equal to the rsaEncryption object identifier defined in [RFC3447], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::rsa_encryption_oid)
            return WebIDL::DataError::create(m_realm, "Algorithm object identifier is not the rsaEncryption object identifier"_string);

        // 5. Let publicKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the subjectPublicKeyInfo field of spki, structure as the RSAPublicKey structure
        //    specified in Section A.1.1 of [RFC3447], and exactData set to true.
        // NOTE: We already did this in parse_a_subject_public_key_info
        auto& public_key = spki.rsa;

        // 6. If an error occurred while parsing, or it can be determined that publicKey is not
        //    a valid public key according to [RFC3447], then throw a DataError.
        // FIXME: Validate the public key

        // 7. Let key be a new CryptoKey that represents the RSA public key identified by publicKey.
        key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

        // 8. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);
    }

    // -> If format is "pkcs8":
    else if (key_format == Bindings::KeyFormat::Pkcs8) {
        // 1. If usages contains an entry which is not "sign" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Sign) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        VERIFY(key_data.has<ByteBuffer>());

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo
        //    is not equal to the rsaEncryption object identifier defined in [RFC3447], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::rsa_encryption_oid)
            return WebIDL::DataError::create(m_realm, "Algorithm object identifier is not the rsaEncryption object identifier"_string);

        // 5. Let rsaPrivateKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the privateKey field of privateKeyInfo, structure as the RSAPrivateKey structure
        //    specified in Section A.1.2 of [RFC3447], and exactData set to true.
        // NOTE: We already did this in parse_a_private_key_info
        auto& rsa_private_key = private_key_info.rsa;

        // 6. If an error occurred while parsing, or if rsaPrivateKey is not
        //    a valid RSA private key according to [RFC3447], then throw a DataError.
        // FIXME: Validate the private key

        // 7. Let key be a new CryptoKey that represents the RSA private key identified by rsaPrivateKey.
        key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { rsa_private_key });

        // 8. Set the [[type]] internal slot of key to "private"
        key->set_type(Bindings::KeyType::Private);
    }

    // -> If format is "jwk":
    else if (key_format == Bindings::KeyFormat::Jwk) {
        // 1. -> If keyData is a JsonWebKey dictionary:
        //         Let jwk equal keyData.
        //    -> Otherwise:
        //         Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "keyData is not a JsonWebKey dictionary"_string);
        auto& jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the d field of jwk is present and usages contains an entry which is not "sign", or,
        //    if the d field of jwk is not present and usages contains an entry which is not "verify"
        //    then throw a SyntaxError.
        if (jwk.d.has_value()) {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Sign) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", Bindings::idl_enum_to_string(usage))));
                }
            }
        } else {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Verify) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", Bindings::idl_enum_to_string(usage))));
                }
            }
        }

        // 3. If the kty field of jwk is not a case-sensitive string match to "RSA", then throw a DataError.
        if (jwk.kty != "RSA"_string)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 4. If usages is non-empty and the use field of jwk is present and is not a case-sensitive string match to "sig", then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && *jwk.use != "sig"_string)
            return WebIDL::DataError::create(m_realm, "Invalid use field"_string);

        // 5. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
        //    or does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(realm, jwk, usages));

        // 6. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !*jwk.ext && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid ext field"_string);

        Optional<String> hash = {};
        // 7. -> If the alg field of jwk is not present:
        if (!jwk.alg.has_value()) {
            //     Let hash be undefined.
        }
        //    ->  If the alg field of jwk is equal to "RS1":
        else if (jwk.alg == "RS1"sv) {
            //     Let hash be the string "SHA-1".
            hash = "SHA-1"_string;
        }
        //    -> If the alg field of jwk is equal to "RS256":
        else if (jwk.alg == "RS256"sv) {
            //     Let hash be the string "SHA-256".
            hash = "SHA-256"_string;
        }
        //    -> If the alg field of jwk is equal to "RS384":
        else if (jwk.alg == "RS384"sv) {
            //     Let hash be the string "SHA-384".
            hash = "SHA-384"_string;
        }
        //    -> If the alg field of jwk is equal to "RS512":
        else if (jwk.alg == "RS512"sv) {
            //     Let hash be the string "SHA-512".
            hash = "SHA-512"_string;
        }
        //    -> Otherwise:
        else {
            // FIXME: Support 'other applicable specifications'
            // 1. Perform any key import steps defined by other applicable specifications, passing format, jwk and obtaining hash.
            // 2. If an error occurred or there are no applicable specifications, throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid alg field"_string);
        }

        // 8. If hash is not undefined:
        if (hash.has_value()) {
            // 1. Let normalizedHash be the result of normalize an algorithm with alg set to hash and op set to digest.
            auto normalized_hash = TRY(normalize_an_algorithm(m_realm, AlgorithmIdentifier { *hash }, "digest"_string));

            // 2. If normalizedHash is not equal to the hash member of normalizedAlgorithm, throw a DataError.
            if (normalized_hash.parameter->name != TRY(normalized_algorithm.hash.name(realm.vm())))
                return WebIDL::DataError::create(m_realm, "Invalid hash"_string);
        }

        // 9. -> If the d field of jwk is present:
        if (jwk.d.has_value()) {
            // 1. If jwk does not meet the requirements of Section 6.3.2 of JSON Web Algorithms [JWA], then throw a DataError.
            bool meets_requirements = jwk.e.has_value() && jwk.n.has_value() && jwk.d.has_value();
            if (jwk.p.has_value() || jwk.q.has_value() || jwk.dp.has_value() || jwk.dq.has_value() || jwk.qi.has_value())
                meets_requirements |= jwk.p.has_value() && jwk.q.has_value() && jwk.dp.has_value() && jwk.dq.has_value() && jwk.qi.has_value();

            if (jwk.oth.has_value()) {
                // FIXME: We don't support > 2 primes in RSA keys
                meets_requirements = false;
            }

            if (!meets_requirements)
                return WebIDL::DataError::create(m_realm, "Invalid JWK private key"_string);

            // 2. Let privateKey represent the RSA private key identified by interpreting jwk according to Section 6.3.2 of JSON Web Algorithms [JWA].
            auto private_key = TRY(parse_jwk_rsa_private_key(realm, jwk));

            // 3. If privateKey can be determined to not be a valid RSA private key according to [RFC3447], then throw a DataError.
            // FIXME: Validate the private key

            // 4. Let key be a new CryptoKey representing privateKey.
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key });

            // 5. Set the [[type]] internal slot of key to "private"
            key->set_type(Bindings::KeyType::Private);
        }

        //     -> Otherwise:
        else {
            // 1. If jwk does not meet the requirements of Section 6.3.1 of JSON Web Algorithms [JWA], then throw a DataError.
            if (!jwk.e.has_value() || !jwk.n.has_value())
                return WebIDL::DataError::create(m_realm, "Invalid JWK public key"_string);

            // 2. Let publicKey represent the RSA public key identified by interpreting jwk according to Section 6.3.1 of JSON Web Algorithms [JWA].
            auto public_key = TRY(parse_jwk_rsa_public_key(realm, jwk));

            // 3. If publicKey can be determined to not be a valid RSA public key according to [RFC3447], then throw a DataError.
            // FIXME: Validate the public key

            // 4. Let key be a new CryptoKey representing publicKey.
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

            // 5. Set the [[type]] internal slot of key to "public"
            key->set_type(Bindings::KeyType::Public);
        }
    }

    // -> Otherwise: throw a NotSupportedError.
    else {
        return WebIDL::NotSupportedError::create(m_realm, "Unsupported key format"_string);
    }

    // 3. Let algorithm be a new RsaHashedKeyAlgorithm.
    auto algorithm = RsaHashedKeyAlgorithm::create(m_realm);

    // 4. Set the name attribute of algorithm to "RSASSA-PKCS1-v1_5"
    algorithm->set_name("RSASSA-PKCS1-v1_5"_string);

    // 5. Set the modulusLength attribute of algorithm to the length, in bits, of the RSA public modulus.
    // 6. Set the publicExponent attribute of algorithm to the BigInteger representation of the RSA public exponent.
    TRY(key->handle().visit(
        [&](::Crypto::PK::RSAPublicKey<> const& public_key) -> WebIDL::ExceptionOr<void> {
            algorithm->set_modulus_length(public_key.modulus().trimmed_byte_length() * 8);
            TRY(algorithm->set_public_exponent(public_key.public_exponent()));
            return {};
        },
        [&](::Crypto::PK::RSAPrivateKey<> const& private_key) -> WebIDL::ExceptionOr<void> {
            algorithm->set_modulus_length(private_key.modulus().trimmed_byte_length() * 8);
            TRY(algorithm->set_public_exponent(private_key.public_exponent()));
            return {};
        },
        [](auto) -> WebIDL::ExceptionOr<void> { VERIFY_NOT_REACHED(); }));

    // 7. Set the hash attribute of algorithm to the hash member of normalizedAlgorithm.
    algorithm->set_hash(normalized_algorithm.hash);

    // 8. Set the [[algorithm]] internal slot of key to algorithm
    key->set_algorithm(algorithm);

    // 9. Return key.
    return GC::Ref { *key };
}

// https://w3c.github.io/webcrypto/#rsassa-pkcs1-operations
WebIDL::ExceptionOr<GC::Ref<JS::Object>> RSASSAPKCS1::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    auto& realm = *m_realm;
    auto& vm = realm.vm();

    // 1. Let key be the key to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    GC::Ptr<JS::Object> result = nullptr;

    // 3. If format is "spki"
    if (format == Bindings::KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(realm, "Key is not public"_string);

        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        // - Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //   - Set the algorithm field to the OID rsaEncryption defined in [RFC3447].
        //   - Set the params field to the ASN.1 type NULL.
        // - Set the subjectPublicKey field to the result of DER-encoding an RSAPublicKey ASN.1 type, as defined in [RFC3447], Appendix A.1.1,
        //   that represents the RSA public key represented by the [[handle]] internal slot of key
        auto maybe_data = handle.visit(
            [&](::Crypto::PK::RSAPublicKey<> const& public_key) -> ErrorOr<ByteBuffer> {
                return TRY(::Crypto::PK::wrap_in_subject_public_key_info(public_key, ::Crypto::ASN1::rsa_encryption_oid, nullptr));
            },
            [](auto) -> ErrorOr<ByteBuffer> {
                VERIFY_NOT_REACHED();
            });
        // FIXME: clang-format butchers the visit if we do the TRY inline
        auto data = TRY_OR_THROW_OOM(vm, maybe_data);

        // 3. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(realm, data);
    }

    // If format is "pkcs8"
    else if (format == Bindings::KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Private)
            return WebIDL::InvalidAccessError::create(realm, "Key is not private"_string);

        // 2. Let data be the result of encoding a privateKeyInfo structure with the following properties:
        // - Set the version field to 0.
        // - Set the privateKeyAlgorithm field to an PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        // - - Set the algorithm field to the OID rsaEncryption defined in [RFC3447].
        // - - Set the params field to the ASN.1 type NULL.
        // - Set the privateKey field to the result of DER-encoding an RSAPrivateKey ASN.1 type, as defined in [RFC3447], Appendix A.1.2,
        // that represents the RSA private key represented by the [[handle]] internal slot of key
        auto maybe_data = handle.visit(
            [&](::Crypto::PK::RSAPrivateKey<> const& private_key) -> ErrorOr<ByteBuffer> {
                return TRY(::Crypto::PK::wrap_in_private_key_info(private_key, ::Crypto::ASN1::rsa_encryption_oid, nullptr));
            },
            [](auto) -> ErrorOr<ByteBuffer> {
                VERIFY_NOT_REACHED();
            });

        // FIXME: clang-format butchers the visit if we do the TRY inline
        auto data = TRY_OR_THROW_OOM(vm, maybe_data);

        // 3. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(realm, data);
    }

    // If format is "jwk"
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to the string "RSA".
        jwk.kty = "RSA"_string;

        // 3. Let hash be the name attribute of the hash attribute of the [[algorithm]] internal slot of key.
        auto hash = TRY(as<RsaHashedKeyAlgorithm>(*key->algorithm()).hash().name(vm));

        // 4. If hash is "SHA-1":
        //      - Set the alg attribute of jwk to the string "RS1".
        if (hash == "SHA-1"sv) {
            jwk.alg = "RS1"_string;
        }
        //    If hash is "SHA-256":
        //      - Set the alg attribute of jwk to the string "RS256".
        else if (hash == "SHA-256"sv) {
            jwk.alg = "RS256"_string;
        }
        //    If hash is "SHA-384":
        //      - Set the alg attribute of jwk to the string "RS384".
        else if (hash == "SHA-384"sv) {
            jwk.alg = "RS384"_string;
        }
        //    If hash is "SHA-512":
        //      - Set the alg attribute of jwk to the string "RS512".
        else if (hash == "SHA-512"sv) {
            jwk.alg = "RS512"_string;
        } else {
            // FIXME: Support 'other applicable specifications'
            // - Perform any key export steps defined by other applicable specifications,
            //   passing format and the hash attribute of the [[algorithm]] internal slot of key and obtaining alg.
            // - Set the alg attribute of jwk to alg.
            return WebIDL::NotSupportedError::create(realm, TRY_OR_THROW_OOM(vm, String::formatted("Unsupported hash algorithm '{}'", hash)));
        }

        // 5. Set the attributes n and e of jwk according to the corresponding definitions in JSON Web Algorithms [JWA], Section 6.3.1.
        auto maybe_error = handle.visit(
            [&](::Crypto::PK::RSAPublicKey<> const& public_key) -> ErrorOr<void> {
                jwk.n = TRY(base64_url_uint_encode(public_key.modulus()));
                jwk.e = TRY(base64_url_uint_encode(public_key.public_exponent()));
                return {};
            },
            // 6. If the [[type]] internal slot of key is "private":
            [&](::Crypto::PK::RSAPrivateKey<> const& private_key) -> ErrorOr<void> {
                jwk.n = TRY(base64_url_uint_encode(private_key.modulus()));
                jwk.e = TRY(base64_url_uint_encode(private_key.public_exponent()));

                // 1. Set the attributes named d, p, q, dp, dq, and qi of jwk according to the corresponding definitions
                //    in JSON Web Algorithms [JWA], Section 6.3.2.
                jwk.d = TRY(base64_url_uint_encode(private_key.private_exponent()));
                jwk.p = TRY(base64_url_uint_encode(private_key.prime1()));
                jwk.q = TRY(base64_url_uint_encode(private_key.prime2()));
                jwk.dp = TRY(base64_url_uint_encode(private_key.exponent1()));
                jwk.dq = TRY(base64_url_uint_encode(private_key.exponent2()));
                jwk.qi = TRY(base64_url_uint_encode(private_key.coefficient()));

                // 2. If the underlying RSA private key represented by the [[handle]] internal slot of key is represented by more than two primes,
                //    set the attribute named oth of jwk according to the corresponding definition in JSON Web Algorithms [JWA], Section 6.3.2.7
                // FIXME: We don't support more than 2 primes on RSA keys
                return {};
            },
            [](auto) -> ErrorOr<void> {
                VERIFY_NOT_REACHED();
            });
        // FIXME: clang-format butchers the visit if we do the TRY inline
        TRY_OR_THROW_OOM(vm, maybe_error);

        // 7. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages()) {
            jwk.key_ops->append(Bindings::idl_enum_to_string(usage));
        }

        // 8. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 9. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        result = TRY(jwk.to_object(realm));
    }

    // Otherwise throw a NotSupportedError.
    else {
        return WebIDL::NotSupportedError::create(realm, TRY_OR_THROW_OOM(vm, String::formatted("Exporting to format {} is not supported", Bindings::idl_enum_to_string(format))));
    }

    // 8. Return result
    return GC::Ref { *result };
}

// https://w3c.github.io/webcrypto/#aes-cbc-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> AesCbc::encrypt(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& plaintext)
{
    auto const& normalized_algorithm = static_cast<AesCbcParams const&>(params);

    // 1. If the iv member of normalizedAlgorithm does not have length 16 bytes, then throw an OperationError.
    if (normalized_algorithm.iv.size() != 16)
        return WebIDL::OperationError::create(m_realm, "IV to AES-CBC must be exactly 16 bytes"_string);

    // 2. Let paddedPlaintext be the result of adding padding octets to the contents of plaintext according to the procedure defined in Section 10.3 of [RFC2315], step 2, with a value of k of 16.
    // 3. Let ciphertext be the result of performing the CBC Encryption operation described in Section 6.2 of [NIST-SP800-38A] using AES as the block cipher, the contents of the iv member of normalizedAlgorithm as the IV input parameter and paddedPlaintext as the input plaintext.
    auto key_bytes = key->handle().get<ByteBuffer>();

    ::Crypto::Cipher::AESCBCCipher cipher(key_bytes);
    auto maybe_ciphertext = cipher.encrypt(plaintext, normalized_algorithm.iv);
    if (maybe_ciphertext.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to encrypt"_string);

    // 4. Return the result of creating an ArrayBuffer containing ciphertext.
    return JS::ArrayBuffer::create(m_realm, maybe_ciphertext.release_value());
}

WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> AesCbc::decrypt(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& ciphertext)
{
    auto const& normalized_algorithm = static_cast<AesCbcParams const&>(params);

    // 1. If the iv member of normalizedAlgorithm does not have length 16 bytes, then throw an OperationError.
    if (normalized_algorithm.iv.size() != 16)
        return WebIDL::OperationError::create(m_realm, "IV to AES-CBC must be exactly 16 bytes"_string);

    // Spec bug? TODO: https://github.com/w3c/webcrypto/issues/381
    // If ciphertext does not have a length that is a multiple of 16 bytes, then throw an OperationError. (Note that a zero-length ciphertext will result in an OperationError in all cases.)
    if (ciphertext.size() % 16 != 0)
        return WebIDL::OperationError::create(m_realm, "Ciphertext length must be a multiple of 16 bytes"_string);

    // 2. Let paddedPlaintext be the result of performing the CBC Decryption operation described in Section 6.2 of [NIST-SP800-38A] using AES as the block cipher, the contents of the iv member of normalizedAlgorithm as the IV input parameter and the contents of ciphertext as the input ciphertext.
    // 3. Let p be the value of the last octet of paddedPlaintext.
    // 4. If p is zero or greater than 16, or if any of the last p octets of paddedPlaintext have a value which is not p, then throw an OperationError.
    // 5. Let plaintext be the result of removing p octets from the end of paddedPlaintext.
    ::Crypto::Cipher::AESCBCCipher cipher(key->handle().get<ByteBuffer>());
    auto maybe_plaintext = cipher.decrypt(ciphertext, normalized_algorithm.iv);
    if (maybe_plaintext.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to decrypt"_string);

    // 6. Return the result of creating an ArrayBuffer containing plaintext.
    return JS::ArrayBuffer::create(m_realm, maybe_plaintext.release_value());
}

// https://w3c.github.io/webcrypto/#aes-cbc-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> AesCbc::import_key(AlgorithmParams const&, Bindings::KeyFormat format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
    for (auto& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Encrypt && usage != Bindings::KeyUsage::Decrypt && usage != Bindings::KeyUsage::Wrapkey && usage != Bindings::KeyUsage::Unwrapkey) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    // 2.
    ByteBuffer data;
    if (format == Bindings::KeyFormat::Raw) {
        // -> If format is "raw":
        //    1. Let data be the octet string contained in keyData.
        //    2. If the length in bits of data is not 128, 192 or 256 then throw a DataError.
        data = key_data.get<ByteBuffer>();
        auto length_in_bits = data.size() * 8;
        if (length_in_bits != 128 && length_in_bits != 192 && length_in_bits != 256) {
            return WebIDL::DataError::create(m_realm, MUST(String::formatted("Invalid key length '{}' bits (must be either 128, 192, or 256 bits)", length_in_bits)));
        }
    } else if (format == Bindings::KeyFormat::Jwk) {
        // -> If format is "jwk":
        //    1. ->   If keyData is a JsonWebKey dictionary:
        //                Let jwk equal keyData.
        //       ->   Otherwise:
        //                Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "keyData is not a JsonWebKey dictionary"_string);
        auto& jwk = key_data.get<Bindings::JsonWebKey>();

        //    2. If the kty field of jwk is not "oct", then throw a DataError.
        if (jwk.kty != "oct"_string)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        //    3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError.
        // Specifically, those requirements are:
        // - ".k" is a valid bas64url encoded octet stream, which we do by just parsing it, in step 4.
        // - ".alg" is checked only in step 5.

        //    4. Let data be the octet string obtained by decoding the k field of jwk.
        data = TRY(parse_jwk_symmetric_key(m_realm, jwk));

        //    5. -> If data has length 128 bits:
        //              If the alg field of jwk is present, and is not "A128CBC", then throw a DataError.
        //       -> If data has length 192 bits:
        //              If the alg field of jwk is present, and is not "A192CBC", then throw a DataError.
        //       -> If data has length 256 bits:
        //              If the alg field of jwk is present, and is not "A256CBC", then throw a DataError.
        //       -> Otherwise:
        //              throw a DataError.
        auto data_bits = data.size() * 8;
        auto const& alg = jwk.alg;
        if (data_bits == 128) {
            if (alg.has_value() && alg != "A128CBC") {
                return WebIDL::DataError::create(m_realm, "Contradictory key size: key has 128 bits, but alg specifies non-128-bit algorithm"_string);
            }
        } else if (data_bits == 192) {
            if (alg.has_value() && alg != "A192CBC") {
                return WebIDL::DataError::create(m_realm, "Contradictory key size: key has 192 bits, but alg specifies non-192-bit algorithm"_string);
            }
        } else if (data_bits == 256) {
            if (alg.has_value() && alg != "A256CBC") {
                return WebIDL::DataError::create(m_realm, "Contradictory key size: key has 256 bits, but alg specifies non-256-bit algorithm"_string);
            }
        } else {
            return WebIDL::DataError::create(m_realm, MUST(String::formatted("Invalid key size: {} bits", data_bits)));
        }

        //    6. If usages is non-empty and the use field of jwk is present and is not "enc", then throw a DataError.
        if (!key_usages.is_empty() && jwk.use.has_value() && *jwk.use != "enc"_string)
            return WebIDL::DataError::create(m_realm, "Invalid use field"_string);

        //    7. If the key_ops field of jwk is present, and is invalid according to the
        //       requirements of JSON Web Key [JWK] or does not contain all of the specified usages
        //       values, then throw a DataError.
        TRY(validate_jwk_key_ops(m_realm, jwk, key_usages));

        //    8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !*jwk.ext && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid ext field"_string);
    } else {
        //    Otherwise:
        //        throw a NotSupportedError
        return WebIDL::NotSupportedError::create(m_realm, "Only raw and jwk formats are supported"_string);
    }

    // 3. Let key be a new CryptoKey object representing an AES key with value data.
    auto data_bits = data.size() * 8;
    auto key = CryptoKey::create(m_realm, move(data));

    // 4. Set the [[type]] internal slot of key to "secret".
    key->set_type(Bindings::KeyType::Secret);

    // 5. Let algorithm be a new AesKeyAlgorithm.
    auto algorithm = AesKeyAlgorithm::create(m_realm);

    // 6. Set the name attribute of algorithm to "AES-CBC".
    algorithm->set_name("AES-CBC"_string);

    // 7. Set the length attribute of algorithm to the length, in bits, of data.
    algorithm->set_length(data_bits);

    // 8. Set the [[algorithm]] internal slot of key to algorithm.
    key->set_algorithm(algorithm);

    // 9. Return key.
    return key;
}

WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> AesCbc::generate_key(AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains any entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Encrypt && usage != Bindings::KeyUsage::Decrypt && usage != Bindings::KeyUsage::Wrapkey && usage != Bindings::KeyUsage::Unwrapkey) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    auto const& normalized_algorithm = static_cast<AesKeyGenParams const&>(params);

    // 2. If the length member of normalizedAlgorithm is not equal to one of 128, 192 or 256, then throw an OperationError.
    auto const bits = normalized_algorithm.length;
    if (bits != 128 && bits != 192 && bits != 256) {
        return WebIDL::OperationError::create(m_realm, MUST(String::formatted("Cannot create AES-CBC key with unusual amount of {} bits", bits)));
    }

    // 3. Generate an AES key of length equal to the length member of normalizedAlgorithm.
    auto key_buffer = TRY(generate_random_key(m_realm->vm(), bits));

    // 4. If the key generation step fails, then throw an OperationError.
    // Note: Cannot happen in our implementation; and if we OOM, then allocating the Exception is probably going to crash anyway.

    // 5. Let key be a new CryptoKey object representing the generated AES key.
    auto key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { key_buffer });

    // 6. Let algorithm be a new AesKeyAlgorithm.
    auto algorithm = AesKeyAlgorithm::create(m_realm);

    // 7. Set the name attribute of algorithm to "AES-CBC".
    algorithm->set_name("AES-CBC"_string);

    // 8. Set the length attribute of algorithm to equal the length member of normalizedAlgorithm.
    algorithm->set_length(bits);

    // 9. Set the [[type]] internal slot of key to "secret".
    key->set_type(Bindings::KeyType::Secret);

    // 10. Set the [[algorithm]] internal slot of key to algorithm.
    key->set_algorithm(algorithm);

    // 11. Set the [[extractable]] internal slot of key to be extractable.
    key->set_extractable(extractable);

    // 12. Set the [[usages]] internal slot of key to be usages.
    key->set_usages(key_usages);

    // 13. Return key.
    return { key };
}

WebIDL::ExceptionOr<GC::Ref<JS::Object>> AesCbc::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    GC::Ptr<JS::Object> result = nullptr;

    // 2. -> If format is "raw":
    if (format == Bindings::KeyFormat::Raw) {
        // 1. Let data be the raw octets of the key represented by [[handle]] internal slot of key.
        auto data = handle.get<ByteBuffer>();

        // 2. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(m_realm, data);
    }
    //    -> If format is "jwk":
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to the string "oct".
        jwk.kty = "oct"_string;

        // 3. Set the k attribute of jwk to be a string containing the raw octets of the key represented by [[handle]] internal slot of key, encoded according to Section 6.4 of JSON Web Algorithms [JWA].
        auto const& key_bytes = handle.get<ByteBuffer>();
        jwk.k = TRY_OR_THROW_OOM(m_realm->vm(), encode_base64url(key_bytes, AK::OmitPadding::Yes));

        // 4. -> If the length attribute of key is 128:
        //        Set the alg attribute of jwk to the string "A128CBC".
        //    -> If the length attribute of key is 192:
        //        Set the alg attribute of jwk to the string "A192CBC".
        //    -> If the length attribute of key is 256:
        //        Set the alg attribute of jwk to the string "A256CBC".
        auto key_bits = key_bytes.size() * 8;
        if (key_bits == 128) {
            jwk.alg = "A128CBC"_string;
        } else if (key_bits == 192) {
            jwk.alg = "A192CBC"_string;
        } else if (key_bits == 256) {
            jwk.alg = "A256CBC"_string;
        } else {
            return WebIDL::OperationError::create(m_realm, "unclear key size"_string);
        }

        // 5. Set the key_ops attribute of jwk to equal the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages()) {
            jwk.key_ops->append(Bindings::idl_enum_to_string(usage));
        }

        // 6. Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 7. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        result = TRY(jwk.to_object(m_realm));
    }
    //    -> Otherwise:
    else {
        //        throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Cannot export to unsupported format"_string);
    }

    // 3. Return result.
    return GC::Ref { *result };
}

WebIDL::ExceptionOr<JS::Value> AesCbc::get_key_length(AlgorithmParams const& params)
{
    // 1. If the length member of normalizedDerivedKeyAlgorithm is not 128, 192 or 256, then throw an OperationError.
    auto const& normalized_algorithm = static_cast<AesDerivedKeyParams const&>(params);
    auto length = normalized_algorithm.length;
    if (length != 128 && length != 192 && length != 256)
        return WebIDL::OperationError::create(m_realm, "Invalid key length"_string);

    // 2. Return the length member of normalizedDerivedKeyAlgorithm.
    return JS::Value(length);
}

WebIDL::ExceptionOr<GC::Ref<CryptoKey>> AesCtr::import_key(AlgorithmParams const&, Bindings::KeyFormat format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
    for (auto& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Encrypt && usage != Bindings::KeyUsage::Decrypt && usage != Bindings::KeyUsage::Wrapkey && usage != Bindings::KeyUsage::Unwrapkey) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    ByteBuffer data;

    // 2. If format is "raw":
    if (format == Bindings::KeyFormat::Raw) {
        // 1. Let data be the octet string contained in keyData.
        data = key_data.get<ByteBuffer>();

        // 2. If the length in bits of data is not 128, 192 or 256 then throw a DataError.
        auto length_in_bits = data.size() * 8;
        if (length_in_bits != 128 && length_in_bits != 192 && length_in_bits != 256) {
            return WebIDL::DataError::create(m_realm, MUST(String::formatted("Invalid key length '{}' bits (must be either 128, 192, or 256 bits)", length_in_bits)));
        }
    }

    // 2. If format is "jwk":
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. -> If keyData is a JsonWebKey dictionary:
        //         Let jwk equal keyData.
        //    -> Otherwise:
        //         Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "keyData is not a JsonWebKey dictionary"_string);

        auto& jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the kty field of jwk is not "oct", then throw a DataError.
        if (jwk.kty != "oct"_string)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError.
        // Specifically, those requirements are:
        // * the member "k" is used to represent a symmetric key (or another key whose value is a single octet sequence).
        // * An "alg" member SHOULD also be present to identify the algorithm intended to be used with the key,
        //   unless the application uses another means or convention to determine the algorithm used.
        if (!jwk.k.has_value())
            return WebIDL::DataError::create(m_realm, "Missing 'k' field"_string);

        if (!jwk.alg.has_value())
            return WebIDL::DataError::create(m_realm, "Missing 'alg' field"_string);

        // 4. Let data be the octet string obtained by decoding the k field of jwk.
        data = TRY(parse_jwk_symmetric_key(m_realm, jwk));

        //    5. -> If data has length 128 bits:
        //              If the alg field of jwk is present, and is not "A128CTR", then throw a DataError.
        //       -> If data has length 192 bits:
        //              If the alg field of jwk is present, and is not "A192CTR", then throw a DataError.
        //       -> If data has length 256 bits:
        //              If the alg field of jwk is present, and is not "A256CTR", then throw a DataError.
        //       -> Otherwise:
        //              throw a DataError.
        auto data_bits = data.size() * 8;
        auto const& alg = jwk.alg;
        if (data_bits == 128) {
            if (alg.has_value() && alg != "A128CTR")
                return WebIDL::DataError::create(m_realm, "Contradictory key size: key has 128 bits, but alg specifies non-128-bit algorithm"_string);
        } else if (data_bits == 192) {
            if (alg.has_value() && alg != "A192CTR")
                return WebIDL::DataError::create(m_realm, "Contradictory key size: key has 192 bits, but alg specifies non-192-bit algorithm"_string);
        } else if (data_bits == 256) {
            if (alg.has_value() && alg != "A256CTR")
                return WebIDL::DataError::create(m_realm, "Contradictory key size: key has 256 bits, but alg specifies non-256-bit algorithm"_string);
        } else {
            return WebIDL::DataError::create(m_realm, MUST(String::formatted("Invalid key size: {} bits", data_bits)));
        }

        // 6. If usages is non-empty and the use field of jwk is present and is not "enc", then throw a DataError.
        if (!key_usages.is_empty() && jwk.use.has_value() && *jwk.use != "enc"_string)
            return WebIDL::DataError::create(m_realm, "Invalid use field"_string);

        // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
        //    or does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(m_realm, jwk, key_usages));

        // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !*jwk.ext && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid ext field"_string);
    }

    // 2. Otherwise:
    else {
        // 1. throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Only raw and jwk formats are supported"_string);
    }

    auto data_bits = data.size() * 8;

    // 3. Let key be a new CryptoKey object representing an AES key with value data.
    auto key = CryptoKey::create(m_realm, move(data));

    // 4. Set the [[type]] internal slot of key to "secret".
    key->set_type(Bindings::KeyType::Secret);

    // 5. Let algorithm be a new AesKeyAlgorithm.
    auto algorithm = AesKeyAlgorithm::create(m_realm);

    // 6. Set the name attribute of algorithm to "AES-CTR".
    algorithm->set_name("AES-CTR"_string);

    // 7. Set the length attribute of algorithm to the length, in bits, of data.
    algorithm->set_length(data_bits);

    // 8. Set the [[algorithm]] internal slot of key to algorithm.
    key->set_algorithm(algorithm);

    // 9. Return key.
    return key;
}

WebIDL::ExceptionOr<GC::Ref<JS::Object>> AesCtr::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible

    GC::Ptr<JS::Object> result = nullptr;

    // 2. If format is "raw":
    if (format == Bindings::KeyFormat::Raw) {
        // 1. Let data be the raw octets of the key represented by [[handle]] internal slot of key.
        auto data = key->handle().get<ByteBuffer>();

        // 2. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(m_realm, data);
    }

    // 2. If format is "jwk":
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to the string "oct".
        jwk.kty = "oct"_string;

        // 3. Set the k attribute of jwk to be a string containing the raw octets of the key represented by [[handle]] internal slot of key,
        //    encoded according to Section 6.4 of JSON Web Algorithms [JWA].
        auto const& key_bytes = key->handle().get<ByteBuffer>();
        jwk.k = TRY_OR_THROW_OOM(m_realm->vm(), encode_base64url(key_bytes, AK::OmitPadding::Yes));

        // 4. -> If the length attribute of key is 128:
        //        Set the alg attribute of jwk to the string "A128CTR".
        //    -> If the length attribute of key is 192:
        //        Set the alg attribute of jwk to the string "A192CTR".
        //    -> If the length attribute of key is 256:
        //        Set the alg attribute of jwk to the string "A256CTR".
        auto key_bits = key_bytes.size() * 8;
        if (key_bits == 128) {
            jwk.alg = "A128CTR"_string;
        } else if (key_bits == 192) {
            jwk.alg = "A192CTR"_string;
        } else if (key_bits == 256) {
            jwk.alg = "A256CTR"_string;
        }

        // 5. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages()) {
            jwk.key_ops->append(Bindings::idl_enum_to_string(usage));
        }

        // 6. Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 7. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        result = TRY(jwk.to_object(m_realm));
    }

    // 2. Otherwise:
    else {
        // 1. throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Cannot export to unsupported format"_string);
    }

    // 3. Return result.
    return GC::Ref { *result };
}

WebIDL::ExceptionOr<JS::Value> AesCtr::get_key_length(AlgorithmParams const& params)
{
    // 1. If the length member of normalizedDerivedKeyAlgorithm is not 128, 192 or 256, then throw a OperationError.
    auto const& normalized_algorithm = static_cast<AesDerivedKeyParams const&>(params);
    auto length = normalized_algorithm.length;
    if (length != 128 && length != 192 && length != 256)
        return WebIDL::OperationError::create(m_realm, "Invalid key length"_string);

    // 2. Return the length member of normalizedDerivedKeyAlgorithm.
    return JS::Value(length);
}

WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> AesCtr::generate_key(AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains any entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Encrypt && usage != Bindings::KeyUsage::Decrypt && usage != Bindings::KeyUsage::Wrapkey && usage != Bindings::KeyUsage::Unwrapkey) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    // 2. If the length member of normalizedAlgorithm is not equal to one of 128, 192 or 256, then throw an OperationError.
    auto const& normalized_algorithm = static_cast<AesKeyGenParams const&>(params);
    auto const bits = normalized_algorithm.length;
    if (bits != 128 && bits != 192 && bits != 256) {
        return WebIDL::OperationError::create(m_realm, MUST(String::formatted("Cannot create AES-CTR key with unusual amount of {} bits", bits)));
    }

    // 3. Generate an AES key of length equal to the length member of normalizedAlgorithm.
    // 4. If the key generation step fails, then throw an OperationError.
    auto key_buffer = TRY(generate_random_key(m_realm->vm(), bits));

    // 5. Let key be a new CryptoKey object representing the generated AES key.
    auto key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { key_buffer });

    // 6. Let algorithm be a new AesKeyAlgorithm.
    auto algorithm = AesKeyAlgorithm::create(m_realm);

    // 7. Set the name attribute of algorithm to "AES-CTR".
    algorithm->set_name("AES-CTR"_string);

    // 8. Set the length attribute of algorithm to equal the length member of normalizedAlgorithm.
    algorithm->set_length(bits);

    // 9. Set the [[type]] internal slot of key to "secret".
    key->set_type(Bindings::KeyType::Secret);

    // 10. Set the [[algorithm]] internal slot of key to algorithm.
    key->set_algorithm(algorithm);

    // 11. Set the [[extractable]] internal slot of key to be extractable.
    key->set_extractable(extractable);

    // 12. Set the [[usages]] internal slot of key to be usages.
    key->set_usages(key_usages);

    // 13. Return key.
    return { key };
}

WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> AesCtr::encrypt(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& plaintext)
{
    // 1. If the counter member of normalizedAlgorithm does not have length 16 bytes, then throw an OperationError.
    auto const& normalized_algorithm = static_cast<AesCtrParams const&>(params);
    auto const& counter = normalized_algorithm.counter;
    if (counter.size() != 16)
        return WebIDL::OperationError::create(m_realm, "Invalid counter length"_string);

    // 2. If the length member of normalizedAlgorithm is zero or is greater than 128, then throw an OperationError.
    auto const& length = normalized_algorithm.length;
    if (length == 0 || length > 128)
        return WebIDL::OperationError::create(m_realm, "Invalid length"_string);

    // 3. Let ciphertext be the result of performing the CTR Encryption operation described in Section 6.5 of [NIST-SP800-38A] using
    //    AES as the block cipher,
    //    the contents of the counter member of normalizedAlgorithm as the initial value of the counter block,
    //    the length member of normalizedAlgorithm as the input parameter m to the standard counter block incrementing function defined in Appendix B.1 of [NIST-SP800-38A]
    //    and the contents of plaintext as the input plaintext.
    ::Crypto::Cipher::AESCTRCipher cipher(key->handle().get<ByteBuffer>());
    auto maybe_ciphertext = cipher.encrypt(plaintext, counter);
    if (maybe_ciphertext.is_error())
        return WebIDL::OperationError::create(m_realm, "Encryption failed"_string);

    // 4. Return the result of creating an ArrayBuffer containing plaintext.
    return JS::ArrayBuffer::create(m_realm, maybe_ciphertext.release_value());
}

WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> AesCtr::decrypt(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& ciphertext)
{
    // 1. If the counter member of normalizedAlgorithm does not have length 16 bytes, then throw an OperationError.
    auto const& normalized_algorithm = static_cast<AesCtrParams const&>(params);
    auto const& counter = normalized_algorithm.counter;
    if (counter.size() != 16)
        return WebIDL::OperationError::create(m_realm, "Invalid counter length"_string);

    // 2. If the length member of normalizedAlgorithm is zero or is greater than 128, then throw an OperationError.
    auto const& length = normalized_algorithm.length;
    if (length == 0 || length > 128)
        return WebIDL::OperationError::create(m_realm, "Invalid length"_string);

    // 3. Let plaintext be the result of performing the CTR Decryption operation described in Section 6.5 of [NIST-SP800-38A] using
    //    AES as the block cipher,
    //    the contents of the counter member of normalizedAlgorithm as the initial value of the counter block,
    //    the length member of normalizedAlgorithm as the input parameter m to the standard counter block incrementing function defined in Appendix B.1 of [NIST-SP800-38A]
    //    and the contents of ciphertext as the input ciphertext.
    ::Crypto::Cipher::AESCTRCipher cipher(key->handle().get<ByteBuffer>());
    auto maybe_plaintext = cipher.decrypt(ciphertext, counter);
    if (maybe_plaintext.is_error())
        return WebIDL::OperationError::create(m_realm, "Decryption failed"_string);

    // 4. Return the result of creating an ArrayBuffer containing plaintext.
    return JS::ArrayBuffer::create(m_realm, maybe_plaintext.release_value());
}

WebIDL::ExceptionOr<JS::Value> AesGcm::get_key_length(AlgorithmParams const& params)
{
    // 1. If the length member of normalizedDerivedKeyAlgorithm is not 128, 192 or 256, then throw a OperationError.
    auto const& normalized_algorithm = static_cast<AesDerivedKeyParams const&>(params);
    auto length = normalized_algorithm.length;
    if (length != 128 && length != 192 && length != 256)
        return WebIDL::OperationError::create(m_realm, "Invalid key length"_string);

    // 2. Return the length member of normalizedDerivedKeyAlgorithm.
    return JS::Value(length);
}

WebIDL::ExceptionOr<GC::Ref<CryptoKey>> AesGcm::import_key(AlgorithmParams const&, Bindings::KeyFormat format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains an entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
    for (auto& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Encrypt && usage != Bindings::KeyUsage::Decrypt && usage != Bindings::KeyUsage::Wrapkey && usage != Bindings::KeyUsage::Unwrapkey) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    ByteBuffer data;

    // 2. If format is "raw":
    if (format == Bindings::KeyFormat::Raw) {
        // 1. Let data be the octet string contained in keyData.
        data = key_data.get<ByteBuffer>();

        // 2. If the length in bits of data is not 128, 192 or 256 then throw a DataError.
        auto length_in_bits = data.size() * 8;
        if (length_in_bits != 128 && length_in_bits != 192 && length_in_bits != 256) {
            return WebIDL::DataError::create(m_realm, MUST(String::formatted("Invalid key length '{}' bits (must be either 128, 192, or 256 bits)", length_in_bits)));
        }
    }

    // 2. If format is "jwk":
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. -> If keyData is a JsonWebKey dictionary:
        //         Let jwk equal keyData.
        //    -> Otherwise:
        //         Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "keyData is not a JsonWebKey dictionary"_string);

        auto& jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the kty field of jwk is not "oct", then throw a DataError.
        if (jwk.kty != "oct"_string)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError.
        // Specifically, those requirements are:
        // * the member "k" is used to represent a symmetric key (or another key whose value is a single octet sequence).
        // * An "alg" member SHOULD also be present to identify the algorithm intended to be used with the key,
        //   unless the application uses another means or convention to determine the algorithm used.
        if (!jwk.k.has_value())
            return WebIDL::DataError::create(m_realm, "Missing 'k' field"_string);

        if (!jwk.alg.has_value())
            return WebIDL::DataError::create(m_realm, "Missing 'alg' field"_string);

        // 4. Let data be the octet string obtained by decoding the k field of jwk.
        data = TRY(parse_jwk_symmetric_key(m_realm, jwk));

        //    5. -> If data has length 128 bits:
        //              If the alg field of jwk is present, and is not "A128GCM", then throw a DataError.
        //       -> If data has length 192 bits:
        //              If the alg field of jwk is present, and is not "A192GCM", then throw a DataError.
        //       -> If data has length 256 bits:
        //              If the alg field of jwk is present, and is not "A256GCM", then throw a DataError.
        //       -> Otherwise:
        //              throw a DataError.
        auto data_bits = data.size() * 8;
        auto const& alg = jwk.alg;
        if (data_bits == 128) {
            if (alg.has_value() && alg != "A128GCM")
                return WebIDL::DataError::create(m_realm, "Contradictory key size: key has 128 bits, but alg specifies non-128-bit algorithm"_string);
        } else if (data_bits == 192) {
            if (alg.has_value() && alg != "A192GCM")
                return WebIDL::DataError::create(m_realm, "Contradictory key size: key has 192 bits, but alg specifies non-192-bit algorithm"_string);
        } else if (data_bits == 256) {
            if (alg.has_value() && alg != "A256GCM")
                return WebIDL::DataError::create(m_realm, "Contradictory key size: key has 256 bits, but alg specifies non-256-bit algorithm"_string);
        } else {
            return WebIDL::DataError::create(m_realm, MUST(String::formatted("Invalid key size: {} bits", data_bits)));
        }

        // 6. If usages is non-empty and the use field of jwk is present and is not "enc", then throw a DataError.
        if (!key_usages.is_empty() && jwk.use.has_value() && *jwk.use != "enc"_string)
            return WebIDL::DataError::create(m_realm, "Invalid use field"_string);

        // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
        //    or does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(m_realm, jwk, key_usages));

        // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !*jwk.ext && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid ext field"_string);
    }

    // 2. Otherwise:
    else {
        // 1. throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Only raw and jwk formats are supported"_string);
    }

    auto data_bits = data.size() * 8;

    // 3. Let key be a new CryptoKey object representing an AES key with value data.
    auto key = CryptoKey::create(m_realm, move(data));

    // 4. Set the [[type]] internal slot of key to "secret".
    key->set_type(Bindings::KeyType::Secret);

    // 5. Let algorithm be a new AesKeyAlgorithm.
    auto algorithm = AesKeyAlgorithm::create(m_realm);

    // 6. Set the name attribute of algorithm to "AES-GCM".
    algorithm->set_name("AES-GCM"_string);

    // 7. Set the length attribute of algorithm to the length, in bits, of data.
    algorithm->set_length(data_bits);

    // 8. Set the [[algorithm]] internal slot of key to algorithm.
    key->set_algorithm(algorithm);

    // 9. Return key.
    return key;
}

WebIDL::ExceptionOr<GC::Ref<JS::Object>> AesGcm::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible

    GC::Ptr<JS::Object> result = nullptr;

    // 2. If format is "raw":
    if (format == Bindings::KeyFormat::Raw) {
        // 1. Let data be the raw octets of the key represented by [[handle]] internal slot of key.
        auto data = key->handle().get<ByteBuffer>();

        // 2. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(m_realm, data);
    }

    // 2. If format is "jwk":
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to the string "oct".
        jwk.kty = "oct"_string;

        // 3. Set the k attribute of jwk to be a string containing the raw octets of the key represented by [[handle]] internal slot of key,
        //    encoded according to Section 6.4 of JSON Web Algorithms [JWA].
        auto const& key_bytes = key->handle().get<ByteBuffer>();
        jwk.k = TRY_OR_THROW_OOM(m_realm->vm(), encode_base64url(key_bytes, AK::OmitPadding::Yes));

        // 4. -> If the length attribute of key is 128:
        //        Set the alg attribute of jwk to the string "A128GCM".
        //    -> If the length attribute of key is 192:
        //        Set the alg attribute of jwk to the string "A192GCM".
        //    -> If the length attribute of key is 256:
        //        Set the alg attribute of jwk to the string "A256GCM".
        auto key_bits = key_bytes.size() * 8;
        if (key_bits == 128) {
            jwk.alg = "A128GCM"_string;
        } else if (key_bits == 192) {
            jwk.alg = "A192GCM"_string;
        } else if (key_bits == 256) {
            jwk.alg = "A256GCM"_string;
        }

        // 5. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages()) {
            jwk.key_ops->append(Bindings::idl_enum_to_string(usage));
        }

        // 6. Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 7. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        result = TRY(jwk.to_object(m_realm));
    }

    // 2. Otherwise:
    else {
        // 1. throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Cannot export to unsupported format"_string);
    }

    // 3. Return result.
    return GC::Ref { *result };
}

WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> AesGcm::encrypt(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& plaintext)
{
    auto const& normalized_algorithm = static_cast<AesGcmParams const&>(params);

    // 1. If plaintext has a length greater than 2^39 - 256 bytes, then throw an OperationError.
    if (plaintext.size() > (1ULL << 39) - 256)
        return WebIDL::OperationError::create(m_realm, "Invalid plaintext length"_string);

    // 2. If the iv member of normalizedAlgorithm has a length greater than 2^64 - 1 bytes, then throw an OperationError.
    // NOTE: This is not possible

    // 3. If the additionalData member of normalizedAlgorithm is present and has a length greater than 2^64 - 1 bytes, then throw an OperationError.
    // NOTE: This is not possible

    // 4. If the tagLength member of normalizedAlgorithm is not present: Let tagLength be 128.
    auto tag_length = 0;
    auto to_compare_against = Vector<int> { 32, 64, 96, 104, 112, 120, 128 };
    if (!normalized_algorithm.tag_length.has_value())
        tag_length = 128;

    // If the tagLength member of normalizedAlgorithm is one of 32, 64, 96, 104, 112, 120 or 128: Let tagLength be equal to the tagLength member of normalizedAlgorithm
    else if (to_compare_against.contains_slow(normalized_algorithm.tag_length.value()))
        tag_length = normalized_algorithm.tag_length.value();

    // Otherwise: throw an OperationError.
    else
        return WebIDL::OperationError::create(m_realm, "Invalid tag length"_string);

    // 5. Let additionalData be the contents of the additionalData member of normalizedAlgorithm if present or the empty octet string otherwise.
    auto additional_data = normalized_algorithm.additional_data.value_or(ByteBuffer {});

    // 6. Let C and T be the outputs that result from performing the Authenticated Encryption Function described in Section 7.1 of [NIST-SP800-38D] using
    //    AES as the block cipher,
    //    the contents of the iv member of normalizedAlgorithm as the IV input parameter,
    //    the contents of additionalData as the A input parameter,
    //    tagLength as the t pre-requisite
    //    and the contents of plaintext as the input plaintext.
    ::Crypto::Cipher::AESGCMCipher cipher(key->handle().get<ByteBuffer>());
    auto maybe_encrypted = cipher.encrypt(plaintext, normalized_algorithm.iv, additional_data, tag_length / 8);
    if (maybe_encrypted.is_error()) {
        return WebIDL::OperationError::create(m_realm, "Encryption failed"_string);
    }

    auto [ciphertext, tag] = maybe_encrypted.release_value();

    // 7. Let ciphertext be equal to C | T, where '|' denotes concatenation.
    TRY_OR_THROW_OOM(m_realm->vm(), ciphertext.try_append(tag));

    // 8. Return the result of creating an ArrayBuffer containing ciphertext.
    return JS::ArrayBuffer::create(m_realm, ciphertext);
}

WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> AesGcm::decrypt(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& ciphertext)
{
    auto const& normalized_algorithm = static_cast<AesGcmParams const&>(params);

    // 1. If the tagLength member of normalizedAlgorithm is not present: Let tagLength be 128.
    u32 tag_length = 0;
    auto to_compare_against = Vector<u32> { 32, 64, 96, 104, 112, 120, 128 };
    if (!normalized_algorithm.tag_length.has_value())
        tag_length = 128;

    // If the tagLength member of normalizedAlgorithm is one of 32, 64, 96, 104, 112, 120 or 128: Let tagLength be equal to the tagLength member of normalizedAlgorithm
    else if (to_compare_against.contains_slow(normalized_algorithm.tag_length.value()))
        tag_length = normalized_algorithm.tag_length.value();

    // Otherwise: throw an OperationError.
    else
        return WebIDL::OperationError::create(m_realm, "Invalid tag length"_string);

    // 2. If ciphertext has a length less than tagLength bits, then throw an OperationError.
    if (ciphertext.size() < tag_length / 8)
        return WebIDL::OperationError::create(m_realm, "Invalid ciphertext length"_string);

    // 3. If the iv member of normalizedAlgorithm has a length greater than 2^64 - 1 bytes, then throw an OperationError.
    // NOTE: This is not possible

    // 4. If the additionalData member of normalizedAlgorithm is present and has a length greater than 2^64 - 1 bytes, then throw an OperationError.
    // NOTE: This is not possible

    // 5. Let tag be the last tagLength bits of ciphertext.
    auto tag_bytes = tag_length / 8;
    auto tag = TRY_OR_THROW_OOM(m_realm->vm(), ciphertext.slice(ciphertext.size() - tag_bytes, tag_bytes));

    // 6. Let actualCiphertext be the result of removing the last tagLength bits from ciphertext.
    auto actual_ciphertext = TRY_OR_THROW_OOM(m_realm->vm(), ciphertext.slice(0, ciphertext.size() - tag_bytes));

    // 7. Let additionalData be the contents of the additionalData member of normalizedAlgorithm if present or the empty octet string otherwise.
    auto additional_data = normalized_algorithm.additional_data.value_or(ByteBuffer {});

    // 8. Perform the Authenticated Decryption Function described in Section 7.2 of [NIST-SP800-38D] using
    //    AES as the block cipher,
    //    the contents of the iv member of normalizedAlgorithm as the IV input parameter,
    //    the contents of additionalData as the A input parameter,
    //    tagLength as the t pre-requisite,
    //    the contents of actualCiphertext as the input ciphertext, C
    //    and the contents of tag as the authentication tag, T.
    // If the result of the algorithm is the indication of inauthenticity, "FAIL": throw an OperationError
    ::Crypto::Cipher::AESGCMCipher cipher(key->handle().get<ByteBuffer>());
    auto maybe_plaintext = cipher.decrypt(actual_ciphertext.bytes(), normalized_algorithm.iv, additional_data, tag);
    if (maybe_plaintext.is_error()) {
        dbgln("FAILED: {}", maybe_plaintext.error());
        return WebIDL::OperationError::create(m_realm, "Decryption failed"_string);
    }

    // Otherwise: Let plaintext be the output P of the Authenticated Decryption Function.
    // 9. Return the result of creating an ArrayBuffer containing plaintext.
    return JS::ArrayBuffer::create(m_realm, maybe_plaintext.release_value());
}

WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> AesGcm::generate_key(AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains any entry which is not one of "encrypt", "decrypt", "wrapKey" or "unwrapKey", then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Encrypt && usage != Bindings::KeyUsage::Decrypt && usage != Bindings::KeyUsage::Wrapkey && usage != Bindings::KeyUsage::Unwrapkey) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    // 2. If the length member of normalizedAlgorithm is not equal to one of 128, 192 or 256, then throw an OperationError.
    auto const& normalized_algorithm = static_cast<AesKeyGenParams const&>(params);
    auto const bits = normalized_algorithm.length;
    if (bits != 128 && bits != 192 && bits != 256) {
        return WebIDL::OperationError::create(m_realm, MUST(String::formatted("Cannot create AES-GCM key with unusual amount of {} bits", bits)));
    }

    // 3. Generate an AES key of length equal to the length member of normalizedAlgorithm.
    // 4. If the key generation step fails, then throw an OperationError.
    auto key_buffer = TRY(generate_random_key(m_realm->vm(), bits));

    // 5. Let key be a new CryptoKey object representing the generated AES key.
    auto key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { key_buffer });

    // 6. Let algorithm be a new AesKeyAlgorithm.
    auto algorithm = AesKeyAlgorithm::create(m_realm);

    // 7. Set the name attribute of algorithm to "AES-GCM".
    algorithm->set_name("AES-GCM"_string);

    // 8. Set the length attribute of algorithm to equal the length member of normalizedAlgorithm.
    algorithm->set_length(bits);

    // 9. Set the [[type]] internal slot of key to "secret".
    key->set_type(Bindings::KeyType::Secret);

    // 10. Set the [[algorithm]] internal slot of key to algorithm.
    key->set_algorithm(algorithm);

    // 11. Set the [[extractable]] internal slot of key to be extractable.
    key->set_extractable(extractable);

    // 12. Set the [[usages]] internal slot of key to be usages.
    key->set_usages(key_usages);

    // 13. Return key.
    return { key };
}

// https://w3c.github.io/webcrypto/#aes-kw-registration
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> AesKw::import_key(AlgorithmParams const&, Bindings::KeyFormat format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains an entry which is not one of "wrapKey" or "unwrapKey", then throw a SyntaxError.
    for (auto& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Wrapkey && usage != Bindings::KeyUsage::Unwrapkey) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    ByteBuffer data;

    // 2. If format is "raw":
    if (format == Bindings::KeyFormat::Raw) {
        // 1. Let data be the octet string contained in keyData.
        data = key_data.get<ByteBuffer>();

        // 2. If the length in bits of data is not 128, 192 or 256 then throw a DataError.
        auto length_in_bits = data.size() * 8;
        if (length_in_bits != 128 && length_in_bits != 192 && length_in_bits != 256) {
            return WebIDL::DataError::create(m_realm, MUST(String::formatted("Invalid key length '{}' bits (must be either 128, 192, or 256 bits)", length_in_bits)));
        }
    }

    // 2. If format is "jwk":
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. -> If keyData is a JsonWebKey dictionary:
        //         Let jwk equal keyData.
        //    -> Otherwise:
        //         Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "keyData is not a JsonWebKey dictionary"_string);

        auto& jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the kty field of jwk is not "oct", then throw a DataError.
        if (jwk.kty != "oct"_string)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA], then throw a DataError.
        // Specifically, those requirements are:
        // * the member "k" is used to represent a symmetric key (or another key whose value is a single octet sequence).
        // * An "alg" member SHOULD also be present to identify the algorithm intended to be used with the key,
        //   unless the application uses another means or convention to determine the algorithm used.
        // NOTE: "k" is already checked in step 4.
        if (!jwk.alg.has_value())
            return WebIDL::DataError::create(m_realm, "Missing 'alg' field"_string);

        // 4. Let data be the octet string obtained by decoding the k field of jwk.
        data = TRY(parse_jwk_symmetric_key(m_realm, jwk));

        // 5. -> If data has length 128 bits:
        //           If the alg field of jwk is present, and is not "A128KW", then throw a DataError.
        //    -> If data has length 192 bits:
        //           If the alg field of jwk is present, and is not "A192KW", then throw a DataError.
        //    -> If data has length 256 bits:
        //           If the alg field of jwk is present, and is not "A256KW", then throw a DataError.
        //    -> Otherwise:
        //           throw a DataError.
        auto data_bits = data.size() * 8;
        auto const& alg = jwk.alg;
        if (data_bits == 128) {
            if (alg.has_value() && alg != "A128KW")
                return WebIDL::DataError::create(m_realm, "Contradictory key size: key has 128 bits, but alg specifies non-128-bit algorithm"_string);
        } else if (data_bits == 192) {
            if (alg.has_value() && alg != "A192KW")
                return WebIDL::DataError::create(m_realm, "Contradictory key size: key has 192 bits, but alg specifies non-192-bit algorithm"_string);
        } else if (data_bits == 256) {
            if (alg.has_value() && alg != "A256KW")
                return WebIDL::DataError::create(m_realm, "Contradictory key size: key has 256 bits, but alg specifies non-256-bit algorithm"_string);
        } else {
            return WebIDL::DataError::create(m_realm, MUST(String::formatted("Invalid key size: {} bits", data_bits)));
        }

        // 6. If usages is non-empty and the use field of jwk is present and is not "enc", then throw a DataError.
        if (!key_usages.is_empty() && jwk.use.has_value() && *jwk.use != "enc"_string)
            return WebIDL::DataError::create(m_realm, "Invalid use field"_string);

        // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK]
        //    or does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(m_realm, jwk, key_usages));

        // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !*jwk.ext && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid ext field"_string);
    }

    // 2. Otherwise:
    else {
        // 1. throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Only raw and jwk formats are supported"_string);
    }

    auto data_bits = data.size() * 8;

    // 3. Let key be a new CryptoKey object representing an AES key with value data.
    auto key = CryptoKey::create(m_realm, move(data));

    // 4. Set the [[type]] internal slot of key to "secret".
    key->set_type(Bindings::KeyType::Secret);

    // 5. Let algorithm be a new AesKeyAlgorithm.
    auto algorithm = AesKeyAlgorithm::create(m_realm);

    // 6. Set the name attribute of algorithm to "AES-KW".
    algorithm->set_name("AES-KW"_string);

    // 7. Set the length attribute of algorithm to the length, in bits, of data.
    algorithm->set_length(data_bits);

    // 8. Set the [[algorithm]] internal slot of key to algorithm.
    key->set_algorithm(algorithm);

    // 9. Return key.
    return key;
}

// https://w3c.github.io/webcrypto/#aes-kw-registration
WebIDL::ExceptionOr<GC::Ref<JS::Object>> AesKw::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible

    GC::Ptr<JS::Object> result = nullptr;

    // 2. If format is "raw":
    if (format == Bindings::KeyFormat::Raw) {
        // 1. Let data be the raw octets of the key represented by [[handle]] internal slot of key.
        auto data = key->handle().get<ByteBuffer>();

        // 2. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(m_realm, data);
    }

    // 2. If format is "jwk":
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to the string "oct".
        jwk.kty = "oct"_string;

        // 3. Set the k attribute of jwk to be a string containing the raw octets of the key represented by [[handle]] internal slot of key,
        //    encoded according to Section 6.4 of JSON Web Algorithms [JWA].
        auto const& key_bytes = key->handle().get<ByteBuffer>();
        jwk.k = TRY_OR_THROW_OOM(m_realm->vm(), encode_base64url(key_bytes, AK::OmitPadding::Yes));

        // 4. -> If the length attribute of key is 128:
        //        Set the alg attribute of jwk to the string "A128KW".
        //    -> If the length attribute of key is 192:
        //        Set the alg attribute of jwk to the string "A192KW".
        //    -> If the length attribute of key is 256:
        //        Set the alg attribute of jwk to the string "A256KW".
        auto key_bits = key_bytes.size() * 8;
        if (key_bits == 128) {
            jwk.alg = "A128KW"_string;
        } else if (key_bits == 192) {
            jwk.alg = "A192KW"_string;
        } else if (key_bits == 256) {
            jwk.alg = "A256KW"_string;
        }

        // 5. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages()) {
            jwk.key_ops->append(Bindings::idl_enum_to_string(usage));
        }

        // 6. Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 7. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        result = TRY(jwk.to_object(m_realm));
    }

    // 2. Otherwise:
    else {
        // 1. throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Cannot export to unsupported format"_string);
    }

    // 3. Return result.
    return GC::Ref { *result };
}

// https://w3c.github.io/webcrypto/#aes-kw-registration
WebIDL::ExceptionOr<JS::Value> AesKw::get_key_length(AlgorithmParams const& params)
{
    // 1. If the length member of normalizedDerivedKeyAlgorithm is not 128, 192 or 256, then throw an OperationError.
    auto const& normalized_algorithm = static_cast<AesDerivedKeyParams const&>(params);
    auto length = normalized_algorithm.length;
    if (length != 128 && length != 192 && length != 256)
        return WebIDL::OperationError::create(m_realm, "Invalid key length"_string);

    // 2. Return the length member of normalizedDerivedKeyAlgorithm.
    return JS::Value(length);
}

// https://w3c.github.io/webcrypto/#aes-kw-registration
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> AesKw::generate_key(AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains any entry which is not one of "wrapKey" or "unwrapKey", then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Wrapkey && usage != Bindings::KeyUsage::Unwrapkey) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    // 2. If the length property of normalizedAlgorithm is not equal to one of 128, 192 or 256, then throw an OperationError.
    auto const& normalized_algorithm = static_cast<AesKeyGenParams const&>(params);
    auto const bits = normalized_algorithm.length;
    if (bits != 128 && bits != 192 && bits != 256) {
        return WebIDL::OperationError::create(m_realm, MUST(String::formatted("Cannot create AES-KW key with unusual amount of {} bits", bits)));
    }

    // 3. Generate an AES key of length equal to the length member of normalizedAlgorithm.
    // 4. If the key generation step fails, then throw an OperationError.
    auto key_buffer = TRY(generate_random_key(m_realm->vm(), bits));

    // 5. Let key be a new CryptoKey object representing the generated AES key.
    auto key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { key_buffer });

    // 6. Let algorithm be a new AesKeyAlgorithm.
    auto algorithm = AesKeyAlgorithm::create(m_realm);

    // 7. Set the name attribute of algorithm to "AES-KW".
    algorithm->set_name("AES-KW"_string);

    // 8. Set the length attribute of algorithm to equal the length member of normalizedAlgorithm.
    algorithm->set_length(bits);

    // 9. Set the [[type]] internal slot of key to "secret".
    key->set_type(Bindings::KeyType::Secret);

    // 10. Set the [[algorithm]] internal slot of key to algorithm.
    key->set_algorithm(algorithm);

    // 11. Set the [[extractable]] internal slot of key to be extractable.
    key->set_extractable(extractable);

    // 12. Set the [[usages]] internal slot of key to be usages.
    key->set_usages(key_usages);

    // 13. Return key.
    return { key };
}

// https://w3c.github.io/webcrypto/#aes-kw-registration
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> AesKw::wrap_key(AlgorithmParams const&, GC::Ref<CryptoKey> key, ByteBuffer const& plaintext)
{
    // 1. If plaintext is not a multiple of 64 bits in length, then throw an OperationError.
    if (plaintext.size() % 8 != 0)
        return WebIDL::OperationError::create(m_realm, "Invalid plaintext length"_string);

    // 2. Let ciphertext be the result of performing the Key Wrap operation described in Section 2.2.1 of [RFC3394]
    //    with plaintext as the plaintext to be wrapped and using the default Initial Value defined in Section 2.2.3.1 of the same document.
    ::Crypto::Cipher::AESKWCipher cipher(key->handle().get<ByteBuffer>());
    auto maybe_ciphertext = cipher.wrap(plaintext.bytes());
    if (maybe_ciphertext.is_error())
        return WebIDL::OperationError::create(m_realm, "Key wrap failed"_string);

    // 3. Return ciphertext.
    return JS::ArrayBuffer::create(m_realm, maybe_ciphertext.release_value());
}

// https://w3c.github.io/webcrypto/#aes-kw-registration
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> AesKw::unwrap_key(AlgorithmParams const&, GC::Ref<CryptoKey> key, ByteBuffer const& ciphertext)
{
    // NOTE: The spec does not mention this, but we need to check
    if (ciphertext.size() < 8)
        return WebIDL::OperationError::create(m_realm, "Invalid ciphertext length"_string);

    // 1. Let plaintext be the result of performing the Key Unwrap operation described in Section 2.2.2 of [RFC3394]
    //     with ciphertext as the input ciphertext and using the default Initial Value defined in Section 2.2.3.1 of the same document
    // 2. If the Key Unwrap operation returns an error, then throw an OperationError.
    ::Crypto::Cipher::AESKWCipher cipher(key->handle().get<ByteBuffer>());
    auto maybe_plaintext = cipher.unwrap(ciphertext.bytes());
    if (maybe_plaintext.is_error())
        return WebIDL::OperationError::create(m_realm, "Key unwrap failed"_string);

    // 3. Return plaintext.
    return JS::ArrayBuffer::create(m_realm, maybe_plaintext.release_value());
}

// https://w3c.github.io/webcrypto/#hkdf-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> HKDF::import_key(AlgorithmParams const&, Bindings::KeyFormat format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. Let keyData be the key data to be imported.

    // 2. If format is "raw":
    //        (… see below …)
    //    Otherwise:
    //        throw a NotSupportedError.
    if (format != Bindings::KeyFormat::Raw) {
        return WebIDL::NotSupportedError::create(m_realm, "Only raw format is supported"_string);
    }

    //        1. If usages contains a value that is not "deriveKey" or "deriveBits", then throw a SyntaxError.
    for (auto& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Derivekey && usage != Bindings::KeyUsage::Derivebits) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    //        2. If extractable is not false, then throw a SyntaxError.
    if (extractable)
        return WebIDL::SyntaxError::create(m_realm, "extractable must be false"_string);

    //        3. Let key be a new CryptoKey representing the key data provided in keyData.
    auto key = CryptoKey::create(m_realm, move(key_data));

    //        4. Set the [[type]] internal slot of key to "secret".
    key->set_type(Bindings::KeyType::Secret);

    //        5. Let algorithm be a new KeyAlgorithm object.
    auto algorithm = KeyAlgorithm::create(m_realm);

    //        6. Set the name attribute of algorithm to "HKDF".
    algorithm->set_name("HKDF"_string);

    //        7. Set the [[algorithm]] internal slot of key to algorithm.
    key->set_algorithm(algorithm);

    //        8. Return key.
    return key;
}

WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> SHA::digest(AlgorithmParams const& algorithm, ByteBuffer const& data)
{
    auto& algorithm_name = algorithm.name;

    ::Crypto::Hash::HashKind hash_kind;
    if (algorithm_name == "SHA-1") {
        hash_kind = ::Crypto::Hash::HashKind::SHA1;
    } else if (algorithm_name == "SHA-256") {
        hash_kind = ::Crypto::Hash::HashKind::SHA256;
    } else if (algorithm_name == "SHA-384") {
        hash_kind = ::Crypto::Hash::HashKind::SHA384;
    } else if (algorithm_name == "SHA-512") {
        hash_kind = ::Crypto::Hash::HashKind::SHA512;
    } else {
        return WebIDL::NotSupportedError::create(m_realm, MUST(String::formatted("Invalid hash function '{}'", algorithm_name)));
    }

    ::Crypto::Hash::Manager hash { hash_kind };
    hash.update(data);

    auto digest = hash.digest();
    auto result_buffer = ByteBuffer::copy(digest.immutable_data(), hash.digest_size());
    if (result_buffer.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to create result buffer"_string);

    return JS::ArrayBuffer::create(m_realm, result_buffer.release_value());
}

// https://w3c.github.io/webcrypto/#ecdsa-operations
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> ECDSA::generate_key(AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains a value which is not one of "sign" or "verify", then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Sign && usage != Bindings::KeyUsage::Verify) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    auto const& normalized_algorithm = static_cast<EcKeyGenParams const&>(params);

    // 2. If the namedCurve member of normalizedAlgorithm is "P-256", "P-384" or "P-521":
    // Generate an Elliptic Curve key pair, as defined in [RFC6090]
    // with domain parameters for the curve identified by the namedCurve member of normalizedAlgorithm.
    Variant<Empty, ::Crypto::Curves::SECP256r1, ::Crypto::Curves::SECP384r1, ::Crypto::Curves::SECP521r1> curve;
    if (normalized_algorithm.named_curve.is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
        if (normalized_algorithm.named_curve == "P-256")
            curve = ::Crypto::Curves::SECP256r1 {};
        else if (normalized_algorithm.named_curve == "P-384")
            curve = ::Crypto::Curves::SECP384r1 {};
        else if (normalized_algorithm.named_curve == "P-521")
            curve = ::Crypto::Curves::SECP521r1 {};
        else
            VERIFY_NOT_REACHED();
    } else {
        // If the namedCurve member of normalizedAlgorithm is a value specified in an applicable specification:
        // Perform the ECDSA generation steps specified in that specification,
        // passing in normalizedAlgorithm and resulting in an elliptic curve key pair.

        // Otherwise: throw a NotSupportedError
        return WebIDL::NotSupportedError::create(m_realm, "Only 'P-256', 'P-384' and 'P-521' is supported"_string);
    }

    // NOTE: Spec jumps to 6 here for some reason
    // 6. If performing the key generation operation results in an error, then throw an OperationError.
    auto maybe_private_key_data = curve.visit(
        [](Empty const&) -> ErrorOr<::Crypto::UnsignedBigInteger> { return Error::from_string_literal("noop error"); },
        [](auto instance) { return instance.generate_private_key(); });

    if (maybe_private_key_data.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to create valid crypto instance"_string);

    auto private_key_data = maybe_private_key_data.release_value();

    auto maybe_public_key_data = curve.visit(
        [](Empty const&) -> ErrorOr<::Crypto::Curves::SECPxxxr1Point> { return Error::from_string_literal("noop error"); },
        [&](auto instance) { return instance.generate_public_key(private_key_data); });

    if (maybe_public_key_data.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to create valid crypto instance"_string);

    auto public_key_data = maybe_public_key_data.release_value();
    auto ec_public_key = ::Crypto::PK::ECPublicKey<> { public_key_data };

    // 7. Let algorithm be a new EcKeyAlgorithm object.
    auto algorithm = EcKeyAlgorithm::create(m_realm);

    // 8. Set the name attribute of algorithm to "ECDSA".
    algorithm->set_name("ECDSA"_string);

    // 9. Set the namedCurve attribute of algorithm to equal the namedCurve member of normalizedAlgorithm.
    algorithm->set_named_curve(normalized_algorithm.named_curve);

    // 10. Let publicKey be a new CryptoKey representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { ec_public_key });

    // 11. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(Bindings::KeyType::Public);

    // 12. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(algorithm);

    // 13. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 14. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
    public_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Verify } }));

    // 15. Let privateKey be a new CryptoKey representing the private key of the generated key pair.
    auto ec_private_key = ::Crypto::PK::ECPrivateKey<> { private_key_data, public_key_data.size, {}, ec_public_key };
    auto private_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { ec_private_key });

    // 16. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(Bindings::KeyType::Private);

    // 17. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(algorithm);

    // 18. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 19. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
    private_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Sign } }));

    // 20. Let result be a new CryptoKeyPair dictionary.
    // 21. Set the publicKey attribute of result to be publicKey.
    // 22. Set the privateKey attribute of result to be privateKey.
    // 23. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
    return Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>> { CryptoKeyPair::create(m_realm, public_key, private_key) };
}

// https://w3c.github.io/webcrypto/#ecdsa-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> ECDSA::sign(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& message)
{
    auto& realm = *m_realm;
    auto& vm = realm.vm();
    auto const& normalized_algorithm = static_cast<EcdsaParams const&>(params);

    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Private)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a private key"_string);

    // 2. Let hashAlgorithm be the hash member of normalizedAlgorithm.
    auto const& hash_algorithm = TRY(normalized_algorithm.hash.name(vm));

    // 3. Let M be the result of performing the digest operation specified by hashAlgorithm using message.
    ::Crypto::Hash::HashKind hash_kind;
    if (hash_algorithm == "SHA-1") {
        hash_kind = ::Crypto::Hash::HashKind::SHA1;
    } else if (hash_algorithm == "SHA-256") {
        hash_kind = ::Crypto::Hash::HashKind::SHA256;
    } else if (hash_algorithm == "SHA-384") {
        hash_kind = ::Crypto::Hash::HashKind::SHA384;
    } else if (hash_algorithm == "SHA-512") {
        hash_kind = ::Crypto::Hash::HashKind::SHA512;
    } else {
        return WebIDL::NotSupportedError::create(m_realm, MUST(String::formatted("Invalid hash function '{}'", hash_algorithm)));
    }
    ::Crypto::Hash::Manager hash { hash_kind };
    hash.update(message);
    auto digest = hash.digest();

    auto M = TRY_OR_THROW_OOM(vm, ByteBuffer::copy(digest.immutable_data(), hash.digest_size()));

    // 4. Let d be the ECDSA private key associated with key.
    auto d = key->handle().get<::Crypto::PK::ECPrivateKey<>>();

    // FIXME: 5. Let params be the EC domain parameters associated with key.

    auto const& internal_algorithm = static_cast<EcKeyAlgorithm const&>(*key->algorithm());
    auto const& named_curve = internal_algorithm.named_curve();

    ByteBuffer result;

    // 6. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
    if (named_curve.is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
        size_t coord_size;
        Variant<Empty, ::Crypto::Curves::SECP256r1, ::Crypto::Curves::SECP384r1, ::Crypto::Curves::SECP521r1> curve;
        if (named_curve == "P-256") {
            coord_size = 256 / 8;
            curve = ::Crypto::Curves::SECP256r1 {};
        } else if (named_curve == "P-384") {
            coord_size = 384 / 8;
            curve = ::Crypto::Curves::SECP384r1 {};
        } else if (named_curve == "P-521") {
            coord_size = ceil_div(521, 8);
            curve = ::Crypto::Curves::SECP521r1 {};
        } else {
            VERIFY_NOT_REACHED();
        }

        // 1. Perform the ECDSA signing process, as specified in [RFC6090], Section 5.4, with M as the message,
        //    using params as the EC domain parameters, and with d as the private key.
        // 2. Let r and s be the pair of integers resulting from performing the ECDSA signing process.
        auto maybe_signature = curve.visit(
            [](Empty const&) -> ErrorOr<::Crypto::Curves::SECPxxxr1Signature> { return Error::from_string_literal("Failed to create valid crypto instance"); },
            [&](auto instance) { return instance.sign(M, d.d()); });

        if (maybe_signature.is_error()) {
            auto error_message = MUST(String::from_utf8(maybe_signature.error().string_literal()));
            return WebIDL::OperationError::create(m_realm, error_message);
        }

        auto signature = maybe_signature.release_value();

        // 3. Let result be an empty byte sequence.
        result = TRY_OR_THROW_OOM(vm, ByteBuffer::create_zeroed(coord_size * 2));

        // 4. Let n be the smallest integer such that n * 8 is greater than the logarithm to base 2 of the order of the base point of the elliptic curve identified by params.
        // 5. Convert r to an octet string of length n and append this sequence of bytes to result.
        auto r_bytes = TRY_OR_THROW_OOM(vm, signature.r_bytes());
        VERIFY(r_bytes.size() <= coord_size);
        result.overwrite(0, r_bytes.data(), r_bytes.size());

        // 6. Convert s to an octet string of length n and append this sequence of bytes to result.
        auto s_bytes = TRY_OR_THROW_OOM(vm, signature.s_bytes());
        VERIFY(s_bytes.size() <= coord_size);
        result.overwrite(coord_size, s_bytes.data(), s_bytes.size());
    } else {
        // FIXME: Otherwise, the namedCurve attribute of the [[algorithm]] internal slot of key is a value specified in an applicable specification:
        // FIXME: Perform the ECDSA signature steps specified in that specification, passing in M, params and d and resulting in result.
    }

    // NOTE: The spec jumps to 9 here for some reason
    // 9. Return the result of creating an ArrayBuffer containing result.
    return JS::ArrayBuffer::create(m_realm, result);
}

// https://w3c.github.io/webcrypto/#ecdsa-operations
WebIDL::ExceptionOr<JS::Value> ECDSA::verify(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& signature, ByteBuffer const& message)
{
    auto& realm = *m_realm;
    auto const& normalized_algorithm = static_cast<EcdsaParams const&>(params);

    // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Public)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a public key"_string);

    // 2. Let hashAlgorithm be the hash member of normalizedAlgorithm.
    [[maybe_unused]] auto const& hash_algorithm = TRY(normalized_algorithm.hash.name(realm.vm()));

    // 3. Let M be the result of performing the digest operation specified by hashAlgorithm using message.
    ::Crypto::Hash::HashKind hash_kind;
    if (hash_algorithm == "SHA-1") {
        hash_kind = ::Crypto::Hash::HashKind::SHA1;
    } else if (hash_algorithm == "SHA-256") {
        hash_kind = ::Crypto::Hash::HashKind::SHA256;
    } else if (hash_algorithm == "SHA-384") {
        hash_kind = ::Crypto::Hash::HashKind::SHA384;
    } else if (hash_algorithm == "SHA-512") {
        hash_kind = ::Crypto::Hash::HashKind::SHA512;
    } else {
        return WebIDL::NotSupportedError::create(m_realm, MUST(String::formatted("Invalid hash function '{}'", hash_algorithm)));
    }
    ::Crypto::Hash::Manager hash { hash_kind };
    hash.update(message);
    auto digest = hash.digest();

    auto M = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::copy(digest.immutable_data(), hash.digest_size()));

    // 4. Let Q be the ECDSA public key associated with key.
    auto Q = key->handle().get<::Crypto::PK::ECPublicKey<>>();

    // FIXME: 5. Let params be the EC domain parameters associated with key.

    // 6. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
    auto const& internal_algorithm = static_cast<EcKeyAlgorithm const&>(*key->algorithm());
    auto const& named_curve = internal_algorithm.named_curve();

    auto result = false;

    Variant<Empty, ::Crypto::Curves::SECP256r1, ::Crypto::Curves::SECP384r1, ::Crypto::Curves::SECP521r1> curve;
    if (named_curve.is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
        if (named_curve == "P-256")
            curve = ::Crypto::Curves::SECP256r1 {};
        else if (named_curve == "P-384")
            curve = ::Crypto::Curves::SECP384r1 {};
        else if (named_curve == "P-521")
            curve = ::Crypto::Curves::SECP521r1 {};
        else
            VERIFY_NOT_REACHED();

        // Perform the ECDSA verifying process, as specified in [RFC6090], Section 5.3,
        // with M as the received message, signature as the received signature
        // and using params as the EC domain parameters, and Q as the public key.

        auto half_size = signature.size() / 2;
        auto r = ::Crypto::UnsignedBigInteger::import_data(signature.data(), half_size);
        auto s = ::Crypto::UnsignedBigInteger::import_data(signature.data() + half_size, half_size);

        auto maybe_result = curve.visit(
            [](Empty const&) -> ErrorOr<bool> { return Error::from_string_literal("Failed to create valid crypto instance"); },
            [&](auto instance) { return instance.verify(M, Q.to_secpxxxr1_point(), ::Crypto::Curves::SECPxxxr1Signature { r, s, half_size }); });

        if (maybe_result.is_error()) {
            auto error_message = MUST(String::from_utf8(maybe_result.error().string_literal()));
            return WebIDL::OperationError::create(m_realm, error_message);
        }

        result = maybe_result.release_value();
    } else {
        // FIXME: Otherwise, the namedCurve attribute of the [[algorithm]] internal slot of key is a value specified in an applicable specification:
        // FIXME: Perform the ECDSA verification steps specified in that specification passing in M, signature, params and Q and resulting in an indication of whether or not the purported signature is valid.
    }

    // 9. Let result be a boolean with the value true if the signature is valid and the value false otherwise.
    // 10. Return result.
    return JS::Value(result);
}

// https://w3c.github.io/webcrypto/#ecdsa-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> ECDSA::import_key(AlgorithmParams const& params, Bindings::KeyFormat key_format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& usages)
{
    // NOTE: This is a parameter to the function
    // 1. Let keyData be the key data to be imported.

    auto const& normalized_algorithm = static_cast<EcKeyImportParams const&>(params);

    GC::Ptr<CryptoKey> key = nullptr;

    // 2. If format is "spki":
    if (key_format == Bindings::KeyFormat::Spki) {
        // 1. If usages contains a value which is not "verify" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Verify) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
        //    is not equal to the id-ecPublicKey object identifier defined in [RFC5480], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::ec_public_key_encryption_oid)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is absent, then throw a DataError.
        if (!spki.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

        // 6. Let params be the parameters field of the algorithm AlgorithmIdentifier field of spki.
        auto ec_params = spki.algorithm.ec_parameters;

        // 7. If params is not an instance of the ECParameters ASN.1 type defined in [RFC5480] that specifies a namedCurve, then throw a DataError.
        // 8. Let namedCurve be a string whose initial value is undefined.
        String named_curve;

        // 9. If params is equivalent to the secp256r1 object identifier defined in [RFC5480]:
        if (ec_params == ::Crypto::ASN1::secp256r1_oid) {
            // Set namedCurve to "P-256".
            named_curve = "P-256"_string;
        }
        // If params is equivalent to the secp384r1 object identifier defined in [RFC5480]:
        else if (ec_params == ::Crypto::ASN1::secp384r1_oid) {
            // Set namedCurve to "P-384".
            named_curve = "P-384"_string;
        }
        // If params is equivalent to the secp521r1 object identifier defined in [RFC5480]:
        else if (ec_params == ::Crypto::ASN1::secp521r1_oid) {
            // Set namedCurve to "P-521".
            named_curve = "P-521"_string;
        }

        // 10. If namedCurve is not undefined
        if (!named_curve.is_empty()) {
            // 1. Let publicKey be the Elliptic Curve public key identified by performing
            //    the conversion steps defined in Section 2.3.4 of [SEC1] to the subjectPublicKey field of spki.
            // The uncompressed point format MUST be supported.
            auto public_key = spki.ec;

            // 2. If the implementation does not support the compressed point format and a compressed point is provided, throw a DataError.
            // 3. If a decode error occurs or an identity point is found, throw a DataError.
            // 4. Let key be a new CryptoKey that represents publicKey.
            // NOTE: We already did this in parse_a_subject_public_key_info
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });
        } else {
            // Otherwise:
            // 1. Perform any key import steps defined by other applicable specifications, passing format, spki and obtaining namedCurve and key.
            // TODO: support 'applicable specifications'

            // 2. If an error occurred or there are no applicable specifications, throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // 11. If namedCurve is defined, and not equal to the namedCurve member of normalizedAlgorithm, throw a DataError.
        if (!named_curve.is_empty() && named_curve != normalized_algorithm.named_curve)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // TODO: 12. If the public key value is not a valid point on the Elliptic Curve identified
        //           by the namedCurve member of normalizedAlgorithm throw a DataError.

        // 13. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);

        // 14. Let algorithm be a new EcKeyAlgorithm.
        auto algorithm = EcKeyAlgorithm::create(m_realm);

        // 15. Set the name attribute of algorithm to "ECDSA".
        algorithm->set_name("ECDSA"_string);

        // 16. Set the namedCurve attribute of algorithm to namedCurve.
        algorithm->set_named_curve(named_curve);

        // 17. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "pkcs8":
    else if (key_format == Bindings::KeyFormat::Pkcs8) {
        // 1. If usages contains a value which is not "sign" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Sign) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo
        //    is not equal to the id-ecPublicKey object identifier defined in [RFC5480], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::ec_public_key_encryption_oid)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field
        //    of privateKeyInfo is not present, then throw a DataError.
        if (!private_key_info.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

        // 6. Let params be the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo.
        auto ec_params = private_key_info.algorithm.ec_parameters;

        // 7. If params is not an instance of the ECParameters ASN.1 type defined in [RFC5480] that specifies a namedCurve, then throw a DataError.
        // 8. Let namedCurve be a string whose initial value is undefined.
        String named_curve;

        // 9. If params is equivalent to the secp256r1 object identifier defined in [RFC5480]:
        if (ec_params == ::Crypto::ASN1::secp256r1_oid) {
            // Set namedCurve to "P-256".
            named_curve = "P-256"_string;
        }
        // If params is equivalent to the secp384r1 object identifier defined in [RFC5480]:
        else if (ec_params == ::Crypto::ASN1::secp384r1_oid) {
            // Set namedCurve to "P-384".
            named_curve = "P-384"_string;
        }
        // If params is equivalent to the secp521r1 object identifier defined in [RFC5480]:
        else if (ec_params == ::Crypto::ASN1::secp521r1_oid) {
            // Set namedCurve to "P-521".
            named_curve = "P-521"_string;
        }

        // 10. If namedCurve is not undefined
        if (!named_curve.is_empty()) {
            // 1. Let ecPrivateKey be the result of performing the parse an ASN.1 structure algorithm,
            //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 ECPrivateKey
            //    structure specified in Section 3 of [RFC5915], and exactData set to true.
            // NOTE: We already did this in parse_a_private_key_info
            // 2. If an error occurred while parsing, then throw a DataError.
            auto& ec_private_key = private_key_info.ec;

            // 3. If the parameters field of ecPrivateKey is present, and is not an instance
            //    of the namedCurve ASN.1 type defined in [RFC5480], or does not contain
            //    the same object identifier as the parameters field of the privateKeyAlgorithm
            //    PrivateKeyAlgorithmIdentifier field of privateKeyInfo, throw a DataError.
            if (ec_private_key.parameters().has_value() && *ec_private_key.parameters() != ec_params.value_or({}))
                return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

            // 4. Let key be a new CryptoKey that represents the Elliptic Curve private key identified
            //    by performing the conversion steps defined in Section 3 of [RFC5915] using ecPrivateKey.
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { ec_private_key });
        } else {
            // Otherwise:
            // 1. Perform any key import steps defined by other applicable specifications, passing format, spki and obtaining namedCurve and key.
            // TODO: support 'applicable specifications'

            // 2. If an error occurred or there are no applicable specifications, throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // 11. If namedCurve is defined, and not equal to the namedCurve member of normalizedAlgorithm, throw a DataError.
        if (!named_curve.is_empty() && named_curve != normalized_algorithm.named_curve)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // TODO: 12. If the key value is not a valid point on the Elliptic Curve identified
        //           by the namedCurve member of normalizedAlgorithm throw a DataError.

        // 13. Set the [[type]] internal slot of key to "private".
        key->set_type(Bindings::KeyType::Private);

        // 14. Let algorithm be a new EcKeyAlgorithm.
        auto algorithm = EcKeyAlgorithm::create(m_realm);

        // 15. Set the name attribute of algorithm to "ECDSA".
        algorithm->set_name("ECDSA"_string);

        // 16. Set the namedCurve attribute of algorithm to namedCurve.
        algorithm->set_named_curve(named_curve);

        // 17. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "jwk":
    else if (key_format == Bindings::KeyFormat::Jwk) {
        // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
        //    Otherwise: Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "keyData is not a JsonWebKey dictionary"_string);
        auto& jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the d field is present and usages contains a value which is not "sign", or,
        //    if the d field is not present and usages contains a value which is not "verify" then throw a SyntaxError.
        if (jwk.d.has_value()) {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Sign) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
                }
            }
        } else {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Verify) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
                }
            }
        }

        // 3. If the kty field of jwk is not "EC", then throw a DataError.
        if (jwk.kty != "EC"sv)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 4. If usages is non-empty and the use field of jwk is present and is not "sig", then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && *jwk.use != "sig"sv)
            return WebIDL::DataError::create(m_realm, "Invalid key use"_string);

        // 5. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
        //    or it does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(m_realm, jwk, usages));

        // 6. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !*jwk.ext && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid extractable"_string);

        // 7. Let namedCurve be a string whose value is equal to the crv field of jwk.
        // NOTE: The spec does not say what to do if crv is missing.
        if (!jwk.crv.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid key crv"_string);
        auto named_curve = *jwk.crv;

        // 8. If namedCurve is not equal to the namedCurve member of normalizedAlgorithm, throw a DataError.
        if (named_curve != normalized_algorithm.named_curve)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // 9. If namedCurve is "P-256", "P-384" or "P-521":
        if (named_curve.is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
            // 1. Let algNamedCurve be a string whose initial value is undefined.
            String alg_named_curve;

            // 2. If the alg field is not present:
            //    Let algNamedCurve be undefined.
            if (jwk.alg.has_value()) {
                // If the alg field is equal to the string "ES256":
                if (*jwk.alg == "ES256")
                    // Let algNamedCurve be the string "P-256".
                    alg_named_curve = "P-256"_string;
                // If the alg field is equal to the string "ES384":
                else if (*jwk.alg == "ES384")
                    // Let algNamedCurve be the string "P-384".
                    alg_named_curve = "P-384"_string;
                // If the alg field is equal to the string "ES512":
                else if (*jwk.alg == "ES512")
                    // Let algNamedCurve be the string "P-521".
                    alg_named_curve = "P-521"_string;
                // otherwise:
                else
                    // throw a DataError.
                    return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
            }

            // 3. If algNamedCurve is defined, and is not equal to namedCurve, throw a DataError.
            if (!alg_named_curve.is_empty() && alg_named_curve != named_curve)
                return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

            size_t coord_size;
            if (named_curve == "P-256"sv)
                coord_size = 256 / 8;
            else if (named_curve == "P-384"sv)
                coord_size = 384 / 8;
            else if (named_curve == "P-521"sv)
                coord_size = ceil_div(521, 8);
            else
                VERIFY_NOT_REACHED();

            // NOTE: according to Section 6.2.1 and 6.2.2 of JSON Web Algorithms [JWA], x and y are always required
            if (!jwk.x.has_value() || !jwk.y.has_value())
                return WebIDL::DataError::create(m_realm, "Invalid key"_string);

            auto maybe_x_bytes = decode_base64url(jwk.x.value());
            if (maybe_x_bytes.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
            }
            auto x_bytes = maybe_x_bytes.release_value();
            if (x_bytes.size() != coord_size)
                return WebIDL::DataError::create(m_realm, "Invalid key size"_string);

            auto maybe_y_bytes = decode_base64url(jwk.y.value());
            if (maybe_y_bytes.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
            }
            auto y_bytes = maybe_y_bytes.release_value();
            if (y_bytes.size() != coord_size)
                return WebIDL::DataError::create(m_realm, "Invalid key size"_string);

            auto public_key = ::Crypto::PK::ECPublicKey<> {
                ::Crypto::UnsignedBigInteger::import_data(x_bytes),
                ::Crypto::UnsignedBigInteger::import_data(y_bytes),
                coord_size,
            };

            // If the d field is present:
            if (jwk.d.has_value()) {
                // 1. If jwk does not meet the requirements of Section 6.2.2 of JSON Web Algorithms [JWA], then throw a DataError.
                auto maybe_d_bytes = decode_base64url(jwk.d.value());
                if (maybe_d_bytes.is_error()) {
                    return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
                }
                auto d_bytes = maybe_d_bytes.release_value();
                if (d_bytes.size() != coord_size)
                    return WebIDL::DataError::create(m_realm, "Invalid key size"_string);

                // 2. Let key be a new CryptoKey object that represents the Elliptic Curve private key identified
                //    by interpreting jwk according to Section 6.2.2 of JSON Web Algorithms [JWA].
                auto private_key = ::Crypto::PK::ECPrivateKey<> {
                    ::Crypto::UnsignedBigInteger::import_data(d_bytes),
                    coord_size,
                    {},
                    public_key,
                };
                key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key });

                // 3. Set the [[type]] internal slot of Key to "private".
                key->set_type(Bindings::KeyType::Private);
            } else {
                // Otherwise:
                // 1. If jwk does not meet the requirements of Section 6.2.1 of JSON Web Algorithms [JWA], then throw a DataError.
                // 2. Let key be a new CryptoKey object that represents the Elliptic Curve public key identified by interpreting
                //    jwk according to Section 6.2.1 of JSON Web Algorithms [JWA].
                key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

                // 3. Set the [[type]] internal slot of Key to "public".
                key->set_type(Bindings::KeyType::Public);
            }
        } else {
            // 1. Perform any key import steps defined by other applicable specifications, passing format, jwk and obtaining key.
            // TODO: support 'applicable specifications'

            // 2. If an error occurred or there are no applicable specifications, throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // TODO: 10. If the key value is not a valid point on the Elliptic Curve identified
        //           by the namedCurve member of normalizedAlgorithm throw a DataError.

        // 11. Let algorithm be a new instance of an EcKeyAlgorithm object.
        auto algorithm = EcKeyAlgorithm::create(m_realm);

        // 12. Set the name attribute of algorithm to "ECDSA".
        algorithm->set_name("ECDSA"_string);

        // 13. Set the namedCurve attribute of algorithm to namedCurve.
        algorithm->set_named_curve(named_curve);

        // 14. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "raw":
    else if (key_format == Bindings::KeyFormat::Raw) {
        // 1. If the namedCurve member of normalizedAlgorithm is not a named curve, then throw a DataError.
        if (!normalized_algorithm.named_curve.is_one_of("P-256"sv, "P-384"sv, "P-521"sv))
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // 2. If usages contains a value which is not "verify" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Verify) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        // 3. If namedCurve is "P-256", "P-384" or "P-521":
        if (normalized_algorithm.named_curve.is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
            auto key_bytes = key_data.get<ByteBuffer>();

            // 1. Let Q be the Elliptic Curve public key on the curve identified by the namedCurve
            //          member of normalizedAlgorithm identified by performing the conversion steps
            //          defined in Section 2.3.4 of [SEC1] to keyData.
            // The uncompressed point format MUST be supported.
            // 2. If the implementation does not support the compressed point format and a compressed point is provided, throw a DataError.
            // 3. If a decode error occurs or an identity point is found, throw a DataError.
            auto maybe_public_key = ::Crypto::PK::EC::parse_ec_key(key_bytes, false, {});
            if (maybe_public_key.is_error())
                return WebIDL::DataError::create(m_realm, "Failed to parse key"_string);

            // 4. Let key be a new CryptoKey that represents Q.
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { maybe_public_key.release_value().public_key });
        } else {
            // Otherwise:
            // 1. Perform any key import steps defined by other applicable specifications, passing format, keyData and obtaining key.
            // TODO: support 'applicable specifications'

            // 2. If an error occured or there are no applicable specifications, throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // 4. Let algorithm be a new EcKeyAlgorithm object.
        auto algorithm = EcKeyAlgorithm::create(m_realm);

        // 5. Set the name attribute of algorithm to "ECDSA".
        algorithm->set_name("ECDSA"_string);

        // 6. Set the namedCurve attribute of algorithm to equal the namedCurve member of normalizedAlgorithm.
        algorithm->set_named_curve(normalized_algorithm.named_curve);

        // 7. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);

        // 8. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // Otherwise:
    else {
        // throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Invalid key format"_string);
    }

    // 3. Return key
    return GC::Ref { *key };
}

// https://w3c.github.io/webcrypto/#ecdsa-operations
WebIDL::ExceptionOr<GC::Ref<JS::Object>> ECDSA::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    // NOTE: This is a parameter to the function
    // 1. Let key be the CryptoKey to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    GC::Ptr<JS::Object> result = nullptr;

    // 3. If format is "spki":
    if (format == Bindings::KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a public key"_string);

        ByteBuffer data;
        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    Set the algorithm field to the OID id-ecPublicKey defined in [RFC5480].
        //    Set the parameters field to an instance of the ECParameters ASN.1 type defined in [RFC5480] as follows:
        //    Set the subjectPublicKey field to keyData
        //    If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
        auto& algorithm = static_cast<EcKeyAlgorithm const&>(*key->algorithm());
        if (algorithm.named_curve().is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
            //  Let keyData be the octet string that represents the Elliptic Curve public key represented by the [[handle]] internal slot
            //  of key according to the encoding rules specified in Section 2.3.3 of [SEC1] and using the uncompressed form.
            //  If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256":
            //      Set parameters to the namedCurve choice with value equal to the object identifier secp256r1 defined in [RFC5480]
            //  If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-384":
            //      Set parameters to the namedCurve choice with value equal to the object identifier secp384r1 defined in [RFC5480]
            //  If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-521":
            //      Set parameters to the namedCurve choice with value equal to the object identifier secp521r1 defined in [RFC5480]
            // NOTE: everything above happens in wrap_in_subject_public_key_info
            auto maybe_data = handle.visit(
                [&](::Crypto::PK::ECPublicKey<> const& public_key) -> ErrorOr<ByteBuffer> {
                    auto public_key_bytes = TRY(public_key.to_uncompressed());

                    Span<int const> ec_params;
                    if (algorithm.named_curve() == "P-256"sv)
                        ec_params = ::Crypto::ASN1::secp256r1_oid;
                    else if (algorithm.named_curve() == "P-384"sv)
                        ec_params = ::Crypto::ASN1::secp384r1_oid;
                    else if (algorithm.named_curve() == "P-521"sv)
                        ec_params = ::Crypto::ASN1::secp521r1_oid;
                    else
                        VERIFY_NOT_REACHED();

                    return TRY(::Crypto::PK::wrap_in_subject_public_key_info(public_key_bytes, ::Crypto::ASN1::ec_public_key_encryption_oid, ec_params));
                },
                [](auto) -> ErrorOr<ByteBuffer> {
                    VERIFY_NOT_REACHED();
                });

            if (maybe_data.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to encode public key"_string);
            }

            data = maybe_data.release_value();
        } else {
            // Otherwise:
            // 1. Perform any key export steps defined by other applicable specifications, passing format and the namedCurve attribute
            //    of the [[algorithm]] internal slot of key and obtaining namedCurveOid and keyData.
            // TODO: support 'applicable specifications'
            // 2. Set parameters to the namedCurve choice with value equal to the object identifier namedCurveOid.
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // NOTE: Spec does not say anything about this.
        result = JS::ArrayBuffer::create(m_realm, data);
    }

    // 3. If format is "pkcs8":
    else if (format == Bindings::KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Private)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a private key"_string);

        ByteBuffer data;

        // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    Set the version field to 0.
        //    Set the privateKeyAlgorithm field to an PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        //      Set the algorithm field to the OID id-ecPublicKey defined in [RFC5480].
        //      Set the privateKey field to keyData.
        //      Set the parameters field to an instance of the ECParameters ASN.1 type defined in [RFC5480] as follows:
        //          If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
        auto& algorithm = static_cast<EcKeyAlgorithm const&>(*key->algorithm());
        if (algorithm.named_curve().is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
            // Let keyData be the result of DER-encoding an instance of the ECPrivateKey structure defined
            // in Section 3 of [RFC5915] for the Elliptic Curve private key represented by the [[handle]] internal slot
            // of key and that conforms to the following:
            //      The parameters field is present, and is equivalent to the parameters field of the privateKeyAlgorithm field
            //      of this PrivateKeyInfo ASN.1 structure.
            //      The publicKey field is present and represents the Elliptic Curve public key associated with the Elliptic Curve
            //      private key represented by the [[handle]] internal slot of key.
            //      If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256":
            //          Set parameters to the namedCurve choice with value equal to the object identifier secp256r1 defined in [RFC5480]
            //      If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-384":
            //          Set parameters to the namedCurve choice with value equal to the object identifier secp384r1 defined in [RFC5480]
            //       If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-521":
            //          Set parameters to the namedCurve choice with value equal to the object identifier secp521r1 defined in [RFC5480]
            // NOTE: everything above happens in wrap_in_private_key_info
            auto maybe_data = handle.visit(
                [&](::Crypto::PK::ECPrivateKey<> const& private_key) -> ErrorOr<ByteBuffer> {
                    Span<int const> ec_params;
                    if (algorithm.named_curve() == "P-256"sv)
                        ec_params = ::Crypto::ASN1::secp256r1_oid;
                    else if (algorithm.named_curve() == "P-384"sv)
                        ec_params = ::Crypto::ASN1::secp384r1_oid;
                    else if (algorithm.named_curve() == "P-521"sv)
                        ec_params = ::Crypto::ASN1::secp521r1_oid;
                    else
                        VERIFY_NOT_REACHED();

                    return TRY(::Crypto::PK::wrap_in_private_key_info(private_key, ::Crypto::ASN1::ec_public_key_encryption_oid, ec_params));
                },
                [](auto) -> ErrorOr<ByteBuffer> {
                    VERIFY_NOT_REACHED();
                });

            if (maybe_data.is_error())
                return WebIDL::DataError::create(m_realm, "Failed to encode private key"_string);

            data = maybe_data.release_value();
        } else {
            // Otherwise:
            // 1. Perform any key export steps defined by other applicable specifications, passing format and the namedCurve attribute
            //    of the [[algorithm]] internal slot of key and obtaining namedCurveOid and keyData.
            // TODO: support 'applicable specifications'
            // 2. Set parameters to the namedCurve choice with value equal to the object identifier namedCurveOid.
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // 3. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(m_realm, data);
    }

    // 3. If format is "jwt":
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to "EC".
        jwk.kty = "EC"_string;

        // 3. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
        auto& algorithm = static_cast<EcKeyAlgorithm const&>(*key->algorithm());
        if (algorithm.named_curve().is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
            // 1. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256":
            if (algorithm.named_curve() == "P-256"sv) {
                // Set the crv attribute of jwk to "P-256"
                jwk.crv = "P-256"_string;
            }
            // If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-384":
            else if (algorithm.named_curve() == "P-384"sv) {
                // Set the crv attribute of jwk to "P-384"
                jwk.crv = "P-384"_string;
            }
            // If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-521":
            else if (algorithm.named_curve() == "P-521"sv) {
                // Set the crv attribute of jwk to "P-521"
                jwk.crv = "P-521"_string;
            }

            auto maybe_error = handle.visit(
                [&](::Crypto::PK::ECPublicKey<> const& public_key) -> ErrorOr<void> {
                    // 2. Set the x attribute of jwk according to the definition in Section 6.2.1.2 of JSON Web Algorithms [JWA].
                    auto x_bytes = TRY(public_key.x_bytes());
                    jwk.x = TRY(encode_base64url(x_bytes, AK::OmitPadding::Yes));

                    // 3. Set the y attribute of jwk according to the definition in Section 6.2.1.3 of JSON Web Algorithms [JWA].
                    auto y_bytes = TRY(public_key.y_bytes());
                    jwk.y = TRY(encode_base64url(y_bytes, AK::OmitPadding::Yes));

                    return {};
                },
                [&](::Crypto::PK::ECPrivateKey<> const& private_key) -> ErrorOr<void> {
                    Variant<Empty, ::Crypto::Curves::SECP256r1, ::Crypto::Curves::SECP384r1, ::Crypto::Curves::SECP521r1> curve;
                    if (algorithm.named_curve() == "P-256"sv)
                        curve = ::Crypto::Curves::SECP256r1 {};
                    else if (algorithm.named_curve() == "P-384"sv)
                        curve = ::Crypto::Curves::SECP384r1 {};
                    else if (algorithm.named_curve() == "P-521"sv)
                        curve = ::Crypto::Curves::SECP521r1 {};
                    else
                        VERIFY_NOT_REACHED();

                    auto maybe_public_key = curve.visit(
                        [](Empty const&) -> ErrorOr<::Crypto::Curves::SECPxxxr1Point> { return Error::from_string_literal("noop error"); },
                        [&](auto instance) { return instance.generate_public_key(private_key.d()); });

                    auto public_key = TRY(maybe_public_key);
                    auto x_bytes = TRY(public_key.x_bytes());
                    auto y_bytes = TRY(public_key.y_bytes());

                    // 2. Set the x attribute of jwk according to the definition in Section 6.2.1.2 of JSON Web Algorithms [JWA].
                    jwk.x = TRY(encode_base64url(x_bytes, AK::OmitPadding::Yes));

                    // 3. Set the y attribute of jwk according to the definition in Section 6.2.1.3 of JSON Web Algorithms [JWA].
                    jwk.y = TRY(encode_base64url(y_bytes, AK::OmitPadding::Yes));

                    return {};
                },
                [](auto) -> ErrorOr<void> {
                    VERIFY_NOT_REACHED();
                });

            if (maybe_error.is_error())
                return WebIDL::DataError::create(m_realm, "Failed to encode public key"_string);

            // 4. If the [[type]] internal slot of key is "private"
            if (key->type() == Bindings::KeyType::Private) {
                auto maybe_error = handle.visit(
                    [&](::Crypto::PK::ECPrivateKey<> const& private_key) -> ErrorOr<void> {
                        // Set the d attribute of jwk according to the definition in Section 6.2.2.1 of JSON Web Algorithms [JWA].
                        auto d_bytes = TRY(private_key.d_bytes());
                        jwk.d = TRY(encode_base64url(d_bytes, AK::OmitPadding::Yes));

                        return {};
                    },
                    [](auto) -> ErrorOr<void> {
                        VERIFY_NOT_REACHED();
                    });

                if (maybe_error.is_error())
                    return WebIDL::DataError::create(m_realm, "Failed to encode private key"_string);
            }
        } else {
            // 1. Perform any key export steps defined by other applicable specifications,
            //    passing format and the namedCurve attribute of the [[algorithm]] internal slot
            //    of key and obtaining namedCurve and a new value of jwk.
            // TODO: support 'applicable specifications'

            // 2. Set the crv attribute of jwk to namedCurve.
            jwk.crv = algorithm.named_curve();
        }

        // 4. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages())
            jwk.key_ops->append(idl_enum_to_string(usage));

        // 5. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 6. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        result = TRY(jwk.to_object(m_realm));
    }

    // 3. If format is "raw":
    else if (format == Bindings::KeyFormat::Raw) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a public key"_string);

        ByteBuffer data;

        // 2. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
        auto& algorithm = static_cast<EcKeyAlgorithm const&>(*key->algorithm());
        if (algorithm.named_curve().is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
            // Let data be an octet string representing the Elliptic Curve point Q represented by [[handle]] internal slot
            // of key according to [SEC1] 2.3.3 using the uncompressed format.
            auto maybe_data = handle.visit(
                [](::Crypto::PK::ECPublicKey<> const& public_key) -> ErrorOr<ByteBuffer> {
                    return public_key.to_uncompressed();
                },
                [](auto) -> ErrorOr<ByteBuffer> {
                    VERIFY_NOT_REACHED();
                });

            if (maybe_data.is_error())
                return WebIDL::DataError::create(m_realm, "Failed to encode public key"_string);

            data = maybe_data.release_value();
        } else {
            // Perform any key export steps defined by other applicable specifications, passing format and
            // the namedCurve attribute of the [[algorithm]] internal slot of key and obtaining namedCurve and data.
            // TODO: support 'applicable specifications'
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // 3. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(m_realm, move(data));
    }

    // Otherwise:
    else {
        // throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Invalid key format"_string);
    }

    // 4. Return result.
    return GC::Ref { *result };
}

// https://w3c.github.io/webcrypto/#ecdh-operations
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> ECDH::generate_key(AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Derivekey && usage != Bindings::KeyUsage::Derivebits) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    auto const& normalized_algorithm = static_cast<EcKeyGenParams const&>(params);

    // 2. If the namedCurve member of normalizedAlgorithm is "P-256", "P-384" or "P-521":
    // Generate an Elliptic Curve key pair, as defined in [RFC6090]
    // with domain parameters for the curve identified by the namedCurve member of normalizedAlgorithm.
    Variant<Empty, ::Crypto::Curves::SECP256r1, ::Crypto::Curves::SECP384r1, ::Crypto::Curves::SECP521r1> curve;
    if (normalized_algorithm.named_curve.is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
        if (normalized_algorithm.named_curve == "P-256")
            curve = ::Crypto::Curves::SECP256r1 {};
        else if (normalized_algorithm.named_curve == "P-384")
            curve = ::Crypto::Curves::SECP384r1 {};
        else if (normalized_algorithm.named_curve == "P-521")
            curve = ::Crypto::Curves::SECP521r1 {};
        else
            VERIFY_NOT_REACHED();
    } else {
        // If the namedCurve member of normalizedAlgorithm is a value specified in an applicable specification
        // that specifies the use of that value with ECDH:
        // Perform the ECDH generation steps specified in that specification,
        // passing in normalizedAlgorithm and resulting in an elliptic curve key pair.

        // Otherwise: throw a NotSupportedError
        return WebIDL::NotSupportedError::create(m_realm, "Only 'P-256', 'P-384' and 'P-521' is supported"_string);
    }

    // 3. If performing the operation results in an error, then throw a OperationError.
    auto maybe_private_key_data = curve.visit(
        [](Empty const&) -> ErrorOr<::Crypto::UnsignedBigInteger> { return Error::from_string_literal("noop error"); },
        [](auto instance) { return instance.generate_private_key(); });

    if (maybe_private_key_data.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to create valid crypto instance"_string);

    auto private_key_data = maybe_private_key_data.release_value();

    auto maybe_public_key_data = curve.visit(
        [](Empty const&) -> ErrorOr<::Crypto::Curves::SECPxxxr1Point> { return Error::from_string_literal("noop error"); },
        [&](auto instance) { return instance.generate_public_key(private_key_data); });

    if (maybe_public_key_data.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to create valid crypto instance"_string);

    auto public_key_data = maybe_public_key_data.release_value();
    auto ec_public_key = ::Crypto::PK::ECPublicKey<> { public_key_data };

    // 4. Let algorithm be a new EcKeyAlgorithm object.
    auto algorithm = EcKeyAlgorithm::create(m_realm);

    // 5. Set the name attribute of algorithm to "ECDH".
    algorithm->set_name("ECDH"_string);

    // 6. Set the namedCurve attribute of algorithm to equal the namedCurve member of normalizedAlgorithm.
    algorithm->set_named_curve(normalized_algorithm.named_curve);

    // 7. Let publicKey be a new CryptoKey representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { ec_public_key });

    // 8. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(Bindings::KeyType::Public);

    // 9. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(algorithm);

    // 10. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 11. Set the [[usages]] internal slot of publicKey to be the empty list.
    public_key->set_usages({});

    // 12. Let privateKey be a new CryptoKey representing the private key of the generated key pair.
    auto ec_private_key = ::Crypto::PK::ECPrivateKey<> { private_key_data, public_key_data.size, {}, ec_public_key };
    auto private_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { ec_private_key });

    // 13. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(Bindings::KeyType::Private);

    // 14. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(algorithm);

    // 15. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 16. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "deriveKey", "deriveBits" ].
    private_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Derivekey, Bindings::KeyUsage::Derivebits } }));

    // 17. Let result be a new CryptoKeyPair dictionary.
    // 18. Set the publicKey attribute of result to be publicKey.
    // 19. Set the privateKey attribute of result to be privateKey.
    // 20. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
    return Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>> { CryptoKeyPair::create(m_realm, public_key, private_key) };
}

// https://w3c.github.io/webcrypto/#ecdh-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> ECDH::derive_bits(AlgorithmParams const& params, GC::Ref<CryptoKey> key, Optional<u32> length_optional)
{
    auto& realm = *m_realm;
    auto const& normalized_algorithm = static_cast<EcdhKeyDeriveParams const&>(params);

    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Private)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a private key"_string);

    // 2. Let publicKey be the public member of normalizedAlgorithm.
    auto& public_key = normalized_algorithm.public_key;

    // 3. If the [[type]] internal slot of publicKey is not "public", then throw an InvalidAccessError.
    if (public_key->type() != Bindings::KeyType::Public)
        return WebIDL::InvalidAccessError::create(realm, "Public key is not a public key"_string);

    // 4. If the name attribute of the [[algorithm]] internal slot of publicKey is not equal to
    //    the name property of the [[algorithm]] internal slot of key, then throw an InvalidAccessError.
    auto& internal_algorithm = static_cast<EcKeyAlgorithm const&>(*key->algorithm());
    auto& public_internal_algorithm = static_cast<EcKeyAlgorithm const&>(*public_key->algorithm());
    if (internal_algorithm.name() != public_internal_algorithm.name()) {
        return WebIDL::InvalidAccessError::create(realm, "Algorithm mismatch"_string);
    }

    // 5. If the namedCurve attribute of the [[algorithm]] internal slot of publicKey is not equal to
    //    the namedCurve property of the [[algorithm]] internal slot of key, then throw an InvalidAccessError.
    if (internal_algorithm.named_curve() != public_internal_algorithm.named_curve())
        return WebIDL::InvalidAccessError::create(realm, "Curve mismatch"_string);

    ByteBuffer secret;

    // 6. If the namedCurve property of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
    // 7. If performing the operation results in an error, then throw a OperationError.
    if (internal_algorithm.named_curve().is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
        // 1. Perform the ECDH primitive specified in [RFC6090] Section 4
        //    with key as the EC private key d and the EC public key represented
        //    by the [[handle]] internal slot of publicKey as the EC public key.
        // 2. Let secret be the result of applying the field element to octet string conversion
        //    defined in Section 6.2 of [RFC6090] to the output of the ECDH primitive.
        auto private_key_data = key->handle().get<::Crypto::PK::ECPrivateKey<>>();
        auto public_key_data = public_key->handle().get<::Crypto::PK::ECPublicKey<>>();

        Variant<Empty, ::Crypto::Curves::SECP256r1, ::Crypto::Curves::SECP384r1, ::Crypto::Curves::SECP521r1> curve;
        if (internal_algorithm.named_curve() == "P-256"sv)
            curve = ::Crypto::Curves::SECP256r1 {};
        else if (internal_algorithm.named_curve() == "P-384"sv)
            curve = ::Crypto::Curves::SECP384r1 {};
        else if (internal_algorithm.named_curve() == "P-521"sv)
            curve = ::Crypto::Curves::SECP521r1 {};
        else
            VERIFY_NOT_REACHED();

        auto maybe_secret = curve.visit(
            [](Empty const&) -> ErrorOr<::Crypto::Curves::SECPxxxr1Point> { return Error::from_string_literal("noop error"); },
            [&private_key_data, &public_key_data](auto instance) { return instance.compute_coordinate(private_key_data.d(), public_key_data.to_secpxxxr1_point()); });

        if (maybe_secret.is_error()) {
            auto message = TRY_OR_THROW_OOM(realm.vm(), String::formatted("Failed to compute secret: {}", maybe_secret.error()));
            return WebIDL::OperationError::create(realm, message);
        }

        auto maybe_secret_data = maybe_secret.release_value().to_uncompressed();
        if (maybe_secret_data.is_error())
            return WebIDL::OperationError::create(realm, "Failed to convert secret"_string);

        // NOTE: Remove the prefix from the uncompressed point
        auto secret_data = maybe_secret_data.release_value();
        VERIFY(secret_data[0] == 0x04);

        // NOTE: Use the x-coordinate as shared secret. RFC6090 section 4.2:
        //   In the ECDH key exchange protocol, after the element g^(j*k) has been
        //   computed, the x-coordinate of that value can be used as the shared secret.
        secret = TRY_OR_THROW_OOM(realm.vm(), secret_data.slice(1, secret_data.size() / 2));
    } else {
        // If the namedCurve property of the [[algorithm]] internal slot of key is a value specified
        // in an applicable specification that specifies the use of that value with ECDH:
        // Perform the ECDH derivation steps specified in that specification,
        // passing in key and publicKey and resulting in secret.
        // TODO: support 'applicable specification'

        return WebIDL::NotSupportedError::create(realm, "Only 'P-256', 'P-384' and 'P-521' is supported"_string);
    }

    // 8. If length is null: Return secret
    if (!length_optional.has_value()) {
        return JS::ArrayBuffer::create(realm, move(secret));
    }

    // Otherwise: If the length of secret in bits is less than length: throw an OperationError.
    auto length = length_optional.value();
    if (secret.size() * 8 < length)
        return WebIDL::OperationError::create(realm, "Secret is too short"_string);

    // Otherwise: Return an octet string containing the first length bits of secret.
    auto slice = TRY_OR_THROW_OOM(realm.vm(), secret.slice(0, ceil_div(length, 8)));
    if (length % 8 != 0) {
        // Zero out the last bits
        slice[slice.size() - 1] &= 0xFF << (8 - (length % 8));
    }

    return JS::ArrayBuffer::create(realm, move(slice));
}

// https://w3c.github.io/webcrypto/#ecdh-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> ECDH::import_key(AlgorithmParams const& params, Bindings::KeyFormat key_format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& usages)
{
    // NOTE: This is a parameter to the function
    // 1. Let keyData be the key data to be imported.

    auto const& normalized_algorithm = static_cast<EcKeyImportParams const&>(params);

    GC::Ptr<CryptoKey> key = nullptr;

    // 2. If format is "spki":
    if (key_format == Bindings::KeyFormat::Spki) {
        // 1. If usages is not empty then throw a SyntaxError.
        if (!usages.is_empty())
            return WebIDL::SyntaxError::create(m_realm, "Usages must be empty"_string);

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
        //    is not equal to the id-ecPublicKey object identifier defined in [RFC5480], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::ec_public_key_encryption_oid)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is absent, then throw a DataError.
        if (!spki.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

        // 6. Let params be the parameters field of the algorithm AlgorithmIdentifier field of spki.
        auto ec_params = spki.algorithm.ec_parameters;

        // 7. If params is not an instance of the ECParameters ASN.1 type defined in [RFC5480] that specifies a namedCurve, then throw a DataError.
        // 8. Let namedCurve be a string whose initial value is undefined.
        String named_curve;

        // 9. If params is equivalent to the secp256r1 object identifier defined in [RFC5480]:
        if (ec_params == ::Crypto::ASN1::secp256r1_oid) {
            // Set namedCurve to "P-256".
            named_curve = "P-256"_string;
        }
        // If params is equivalent to the secp384r1 object identifier defined in [RFC5480]:
        else if (ec_params == ::Crypto::ASN1::secp384r1_oid) {
            // Set namedCurve to "P-384".
            named_curve = "P-384"_string;
        }
        // If params is equivalent to the secp521r1 object identifier defined in [RFC5480]:
        else if (ec_params == ::Crypto::ASN1::secp521r1_oid) {
            // Set namedCurve to "P-521".
            named_curve = "P-521"_string;
        }

        // 10. If namedCurve is not undefined
        if (!named_curve.is_empty()) {
            // 1. Let publicKey be the Elliptic Curve public key identified by performing
            //    the conversion steps defined in Section 2.3.4 of [SEC1] to the subjectPublicKey field of spki.
            // The uncompressed point format MUST be supported.
            auto public_key = spki.ec;

            // 2. If the implementation does not support the compressed point format and a compressed point is provided, throw a DataError.
            // 3. If a decode error occurs or an identity point is found, throw a DataError.
            // 4. Let key be a new CryptoKey that represents publicKey.
            // NOTE: We already did this in parse_a_subject_public_key_info
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });
        } else {
            // Otherwise:
            // 1. Perform any key import steps defined by other applicable specifications, passing format, spki and obtaining namedCurve and key.
            // TODO: support 'applicable specifications'

            // 2. If an error occurred or there are no applicable specifications, throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // 11. If namedCurve is defined, and not equal to the namedCurve member of normalizedAlgorithm, throw a DataError.
        if (!named_curve.is_empty() && named_curve != normalized_algorithm.named_curve)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // TODO: 12. If the key value is not a valid point on the Elliptic Curve identified
        //           by the namedCurve member of normalizedAlgorithm throw a DataError.

        // 13. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);

        // 14. Let algorithm be a new EcKeyAlgorithm.
        auto algorithm = EcKeyAlgorithm::create(m_realm);

        // 15. Set the name attribute of algorithm to "ECDH".
        algorithm->set_name("ECDH"_string);

        // 16. Set the namedCurve attribute of algorithm to namedCurve.
        algorithm->set_named_curve(named_curve);

        // 17. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "pkcs8":
    else if (key_format == Bindings::KeyFormat::Pkcs8) {
        // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Derivekey && usage != Bindings::KeyUsage::Derivebits) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo
        //    is not equal to the id-ecPublicKey object identifier defined in [RFC5480], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::ec_public_key_encryption_oid)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field
        //    of privateKeyInfo is not present, then throw a DataError.
        if (!private_key_info.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

        // 6. Let params be the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo.
        auto ec_params = private_key_info.algorithm.ec_parameters;

        // 7. If params is not an instance of the ECParameters ASN.1 type defined in [RFC5480] that specifies a namedCurve, then throw a DataError.
        // 8. Let namedCurve be a string whose initial value is undefined.
        String named_curve;

        // 9. If params is equivalent to the secp256r1 object identifier defined in [RFC5480]:
        if (ec_params == ::Crypto::ASN1::secp256r1_oid) {
            // Set namedCurve to "P-256".
            named_curve = "P-256"_string;
        }
        // If params is equivalent to the secp384r1 object identifier defined in [RFC5480]:
        else if (ec_params == ::Crypto::ASN1::secp384r1_oid) {
            // Set namedCurve to "P-384".
            named_curve = "P-384"_string;
        }
        // If params is equivalent to the secp521r1 object identifier defined in [RFC5480]:
        else if (ec_params == ::Crypto::ASN1::secp521r1_oid) {
            // Set namedCurve to "P-521".
            named_curve = "P-521"_string;
        }

        // 10. If namedCurve is not undefined
        if (!named_curve.is_empty()) {
            // 1. Let ecPrivateKey be the result of performing the parse an ASN.1 structure algorithm,
            //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 ECPrivateKey
            //    structure specified in Section 3 of [RFC5915], and exactData set to true.
            // NOTE: We already did this in parse_a_private_key_info
            // 2. If an error occurred while parsing, then throw a DataError.
            auto& ec_private_key = private_key_info.ec;

            // 3. If the parameters field of ecPrivateKey is present, and is not an instance
            //    of the namedCurve ASN.1 type defined in [RFC5480], or does not contain
            //    the same object identifier as the parameters field of the privateKeyAlgorithm
            //    PrivateKeyAlgorithmIdentifier field of privateKeyInfo, throw a DataError.
            if (ec_private_key.parameters().has_value() && *ec_private_key.parameters() != ec_params.value_or({}))
                return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

            // 4. Let key be a new CryptoKey that represents the Elliptic Curve private key identified
            //    by performing the conversion steps defined in Section 3 of [RFC5915] using ecPrivateKey.
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { ec_private_key });
        } else {
            // Otherwise:
            // 1. Perform any key import steps defined by other applicable specifications, passing format, spki and obtaining namedCurve and key.
            // TODO: support 'applicable specifications'

            // 2. If an error occurred or there are no applicable specifications, throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // 11. If namedCurve is defined, and not equal to the namedCurve member of normalizedAlgorithm, throw a DataError.
        if (!named_curve.is_empty() && named_curve != normalized_algorithm.named_curve)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // TODO: 12. If the key value is not a valid point on the Elliptic Curve identified
        //           by the namedCurve member of normalizedAlgorithm throw a DataError.

        // 13. Set the [[type]] internal slot of key to "private".
        key->set_type(Bindings::KeyType::Private);

        // 14. Let algorithm be a new EcKeyAlgorithm.
        auto algorithm = EcKeyAlgorithm::create(m_realm);

        // 15. Set the name attribute of algorithm to "ECDH".
        algorithm->set_name("ECDH"_string);

        // 16. Set the namedCurve attribute of algorithm to namedCurve.
        algorithm->set_named_curve(named_curve);

        // 17. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "jwk":
    else if (key_format == Bindings::KeyFormat::Jwk) {
        // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
        //    Otherwise: Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "keyData is not a JsonWebKey dictionary"_string);
        auto& jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the d field is present and if usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        if (jwk.d.has_value() && !usages.is_empty()) {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Derivekey && usage != Bindings::KeyUsage::Derivebits) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
                }
            }
        }

        // 3. If the d field is not present and if usages is not empty then throw a SyntaxError.
        if (!jwk.d.has_value() && !usages.is_empty())
            return WebIDL::SyntaxError::create(m_realm, "Usages must be empty"_string);

        // 4. If the kty field of jwk is not "EC", then throw a DataError.
        if (jwk.kty != "EC"sv)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 5. If usages is non-empty and the use field of jwk is present and is not equal to "enc" then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && *jwk.use != "enc"sv)
            return WebIDL::DataError::create(m_realm, "Invalid key use"_string);

        // 6. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
        //    or it does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(m_realm, jwk, usages));

        // 7. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !*jwk.ext && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid extractable"_string);

        // 8. Let namedCurve be a string whose value is equal to the crv field of jwk.
        // NOTE: The spec does not say what to do if crv is missing.
        if (!jwk.crv.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid key crv"_string);
        auto named_curve = *jwk.crv;

        // 9. If namedCurve is not equal to the namedCurve member of normalizedAlgorithm, throw a DataError.
        if (named_curve != normalized_algorithm.named_curve)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // 10. If namedCurve is "P-256", "P-384" or "P-521":
        if (named_curve.is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
            size_t coord_size;
            if (named_curve == "P-256"sv)
                coord_size = 256 / 8;
            else if (named_curve == "P-384"sv)
                coord_size = 384 / 8;
            else if (named_curve == "P-521"sv)
                coord_size = ceil_div(521, 8);
            else
                VERIFY_NOT_REACHED();

            // NOTE: according to Section 6.2.1 and 6.2.2 of JSON Web Algorithms [JWA], x and y are always required
            if (!jwk.x.has_value() || !jwk.y.has_value())
                return WebIDL::DataError::create(m_realm, "Invalid key"_string);

            auto maybe_x_bytes = decode_base64url(jwk.x.value());
            if (maybe_x_bytes.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
            }
            auto x_bytes = maybe_x_bytes.release_value();
            if (x_bytes.size() != coord_size)
                return WebIDL::DataError::create(m_realm, "Invalid key size"_string);

            auto maybe_y_bytes = decode_base64url(jwk.y.value());
            if (maybe_y_bytes.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
            }
            auto y_bytes = maybe_y_bytes.release_value();
            if (y_bytes.size() != coord_size)
                return WebIDL::DataError::create(m_realm, "Invalid key size"_string);

            auto public_key = ::Crypto::PK::ECPublicKey<> {
                ::Crypto::UnsignedBigInteger::import_data(x_bytes),
                ::Crypto::UnsignedBigInteger::import_data(y_bytes),
                coord_size,
            };

            // If the d field is present:
            if (jwk.d.has_value()) {
                // 1. If jwk does not meet the requirements of Section 6.2.2 of JSON Web Algorithms [JWA], then throw a DataError.
                auto maybe_d_bytes = decode_base64url(jwk.d.value());
                if (maybe_d_bytes.is_error()) {
                    return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
                }
                auto d_bytes = maybe_d_bytes.release_value();
                if (d_bytes.size() != coord_size)
                    return WebIDL::DataError::create(m_realm, "Invalid key size"_string);

                // 2. Let key be a new CryptoKey object that represents the Elliptic Curve private key identified
                //    by interpreting jwk according to Section 6.2.2 of JSON Web Algorithms [JWA].
                auto private_key = ::Crypto::PK::ECPrivateKey<> {
                    ::Crypto::UnsignedBigInteger::import_data(d_bytes),
                    coord_size,
                    {},
                    public_key,
                };
                key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key });

                // 3. Set the [[type]] internal slot of Key to "private".
                key->set_type(Bindings::KeyType::Private);
            } else {
                // Otherwise:
                // 1. If jwk does not meet the requirements of Section 6.2.1 of JSON Web Algorithms [JWA], then throw a DataError.
                // 2. Let key be a new CryptoKey object that represents the Elliptic Curve public key identified by interpreting
                //    jwk according to Section 6.2.1 of JSON Web Algorithms [JWA].
                key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

                // 3. Set the [[type]] internal slot of Key to "public".
                key->set_type(Bindings::KeyType::Public);
            }
        } else {
            // 1. Perform any key import steps defined by other applicable specifications, passing format, jwk and obtaining key.
            // TODO: support 'applicable specifications'

            // 2. If an error occurred or there are no applicable specifications, throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // TODO: 11. If the key value is not a valid point on the Elliptic Curve identified
        //           by the namedCurve member of normalizedAlgorithm throw a DataError.

        // 12. Let algorithm be a new instance of an EcKeyAlgorithm object.
        auto algorithm = EcKeyAlgorithm::create(m_realm);

        // 13. Set the name attribute of algorithm to "ECDH".
        algorithm->set_name("ECDH"_string);

        // 14. Set the namedCurve attribute of algorithm to namedCurve.
        algorithm->set_named_curve(named_curve);

        // 15. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "raw":
    else if (key_format == Bindings::KeyFormat::Raw) {
        // 1. If the namedCurve member of normalizedAlgorithm is not a named curve, then throw a DataError.
        if (!normalized_algorithm.named_curve.is_one_of("P-256"sv, "P-384"sv, "P-521"sv))
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // 2. If usages is not the empty list, then throw a SyntaxError.
        if (!usages.is_empty())
            return WebIDL::SyntaxError::create(m_realm, "Usages must be empty"_string);

        // 3. If namedCurve is "P-256", "P-384" or "P-521":
        if (normalized_algorithm.named_curve.is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
            auto key_bytes = key_data.get<ByteBuffer>();

            // 1. Let Q be the Elliptic Curve public key on the curve identified by the namedCurve
            //          member of normalizedAlgorithm identified by performing the conversion steps
            //          defined in Section 2.3.4 of [SEC1] to keyData.
            // The uncompressed point format MUST be supported.
            // 2. If the implementation does not support the compressed point format and a compressed point is provided, throw a DataError.
            // 3. If a decode error occurs or an identity point is found, throw a DataError.
            auto maybe_public_key = ::Crypto::PK::EC::parse_ec_key(key_bytes, false, {});
            if (maybe_public_key.is_error())
                return WebIDL::DataError::create(m_realm, "Failed to parse key"_string);

            // 4. Let key be a new CryptoKey that represents Q.
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { maybe_public_key.release_value().public_key });
        } else {
            // Otherwise:
            // 1. Perform any key import steps defined by other applicable specifications, passing format, keyData and obtaining key.
            // TODO: support 'applicable specifications'

            // 2. If an error occured or there are no applicable specifications, throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // 4. Let algorithm be a new EcKeyAlgorithm object.
        auto algorithm = EcKeyAlgorithm::create(m_realm);

        // 5. Set the name attribute of algorithm to "ECDH".
        algorithm->set_name("ECDH"_string);

        // 6. Set the namedCurve attribute of algorithm to equal the namedCurve member of normalizedAlgorithm.
        algorithm->set_named_curve(normalized_algorithm.named_curve);

        // 7. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);

        // 8. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 3. Return key
    return GC::Ref { *key };
}

// https://w3c.github.io/webcrypto/#ecdh-operations
WebIDL::ExceptionOr<GC::Ref<JS::Object>> ECDH::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    // NOTE: This is a parameter to the function
    // 1. Let key be the CryptoKey to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    GC::Ptr<JS::Object> result = nullptr;

    // 3. If format is "spki":
    if (format == Bindings::KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a public key"_string);

        ByteBuffer data;
        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    Set the algorithm field to the OID id-ecPublicKey defined in [RFC5480].
        //    Set the parameters field to an instance of the ECParameters ASN.1 type defined in [RFC5480] as follows:
        //    Set the subjectPublicKey field to keyData
        //    If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
        auto& algorithm = static_cast<EcKeyAlgorithm const&>(*key->algorithm());
        if (algorithm.named_curve().is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
            //  Let keyData be the octet string that represents the Elliptic Curve public key represented by the [[handle]] internal slot
            //  of key according to the encoding rules specified in Section 2.3.3 of [SEC1] and using the uncompressed form.
            //  If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256":
            //      Set parameters to the namedCurve choice with value equal to the object identifier secp256r1 defined in [RFC5480]
            //  If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-384":
            //      Set parameters to the namedCurve choice with value equal to the object identifier secp384r1 defined in [RFC5480]
            //  If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-521":
            //      Set parameters to the namedCurve choice with value equal to the object identifier secp521r1 defined in [RFC5480]
            // NOTE: everything above happens in wrap_in_subject_public_key_info
            auto maybe_data = handle.visit(
                [&](::Crypto::PK::ECPublicKey<> const& public_key) -> ErrorOr<ByteBuffer> {
                    auto public_key_bytes = TRY(public_key.to_uncompressed());

                    Span<int const> ec_params;
                    if (algorithm.named_curve() == "P-256"sv)
                        ec_params = ::Crypto::ASN1::secp256r1_oid;
                    else if (algorithm.named_curve() == "P-384"sv)
                        ec_params = ::Crypto::ASN1::secp384r1_oid;
                    else if (algorithm.named_curve() == "P-521"sv)
                        ec_params = ::Crypto::ASN1::secp521r1_oid;
                    else
                        VERIFY_NOT_REACHED();

                    return TRY(::Crypto::PK::wrap_in_subject_public_key_info(public_key_bytes, ::Crypto::ASN1::ec_public_key_encryption_oid, ec_params));
                },
                [](auto) -> ErrorOr<ByteBuffer> {
                    VERIFY_NOT_REACHED();
                });

            if (maybe_data.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to encode public key"_string);
            }

            data = maybe_data.release_value();
        } else {
            // Otherwise:
            // 1. Perform any key export steps defined by other applicable specifications, passing format and the namedCurve attribute
            //    of the [[algorithm]] internal slot of key and obtaining namedCurveOid and keyData.
            // TODO: support 'applicable specifications'
            // 2. Set parameters to the namedCurve choice with value equal to the object identifier namedCurveOid.
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // NOTE: Spec does not say anything about this.
        result = JS::ArrayBuffer::create(m_realm, data);
    }

    // 3. If format is "pkcs8":
    else if (format == Bindings::KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Private)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a private key"_string);

        ByteBuffer data;

        // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    Set the version field to 0.
        //    Set the privateKeyAlgorithm field to an PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        //      Set the algorithm field to the OID id-ecPublicKey defined in [RFC5480].
        //      Set the privateKey field to keyData.
        //      Set the parameters field to an instance of the ECParameters ASN.1 type defined in [RFC5480] as follows:
        //          If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
        auto& algorithm = static_cast<EcKeyAlgorithm const&>(*key->algorithm());
        if (algorithm.named_curve().is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
            // Let keyData be the result of DER-encoding an instance of the ECPrivateKey structure defined
            // in Section 3 of [RFC5915] for the Elliptic Curve private key represented by the [[handle]] internal slot
            // of key and that conforms to the following:
            //      The parameters field is present, and is equivalent to the parameters field of the privateKeyAlgorithm field
            //      of this PrivateKeyInfo ASN.1 structure.
            //      The publicKey field is present and represents the Elliptic Curve public key associated with the Elliptic Curve
            //      private key represented by the [[handle]] internal slot of key.
            //      If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256":
            //          Set parameters to the namedCurve choice with value equal to the object identifier secp256r1 defined in [RFC5480]
            //      If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-384":
            //          Set parameters to the namedCurve choice with value equal to the object identifier secp384r1 defined in [RFC5480]
            //       If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-521":
            //          Set parameters to the namedCurve choice with value equal to the object identifier secp521r1 defined in [RFC5480]
            // NOTE: everything above happens in wrap_in_private_key_info
            auto maybe_data = handle.visit(
                [&](::Crypto::PK::ECPrivateKey<> const& private_key) -> ErrorOr<ByteBuffer> {
                    Span<int const> ec_params;
                    if (algorithm.named_curve() == "P-256"sv)
                        ec_params = ::Crypto::ASN1::secp256r1_oid;
                    else if (algorithm.named_curve() == "P-384"sv)
                        ec_params = ::Crypto::ASN1::secp384r1_oid;
                    else if (algorithm.named_curve() == "P-521"sv)
                        ec_params = ::Crypto::ASN1::secp521r1_oid;
                    else
                        VERIFY_NOT_REACHED();

                    return TRY(::Crypto::PK::wrap_in_private_key_info(private_key, ::Crypto::ASN1::ec_public_key_encryption_oid, ec_params));
                },
                [](auto) -> ErrorOr<ByteBuffer> {
                    VERIFY_NOT_REACHED();
                });

            if (maybe_data.is_error())
                return WebIDL::DataError::create(m_realm, "Failed to encode private key"_string);

            data = maybe_data.release_value();
        } else {
            // Otherwise:
            // 1. Perform any key export steps defined by other applicable specifications, passing format and the namedCurve attribute
            //    of the [[algorithm]] internal slot of key and obtaining namedCurveOid and keyData.
            // TODO: support 'applicable specifications'
            // 2. Set parameters to the namedCurve choice with value equal to the object identifier namedCurveOid.
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // NOTE: Spec does not say anything about this.
        result = JS::ArrayBuffer::create(m_realm, data);
    }

    // 3. If format is "jwt":
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to "EC".
        jwk.kty = "EC"_string;

        // 3. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
        auto& algorithm = static_cast<EcKeyAlgorithm const&>(*key->algorithm());
        if (algorithm.named_curve().is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
            // 1. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256":
            if (algorithm.named_curve() == "P-256"sv) {
                // Set the crv attribute of jwk to "P-256"
                jwk.crv = "P-256"_string;
            }
            // If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-384":
            else if (algorithm.named_curve() == "P-384"sv) {
                // Set the crv attribute of jwk to "P-384"
                jwk.crv = "P-384"_string;
            }
            // If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-521":
            else if (algorithm.named_curve() == "P-521"sv) {
                // Set the crv attribute of jwk to "P-521"
                jwk.crv = "P-521"_string;
            }

            auto maybe_error = handle.visit(
                [&](::Crypto::PK::ECPublicKey<> const& public_key) -> ErrorOr<void> {
                    // 2. Set the x attribute of jwk according to the definition in Section 6.2.1.2 of JSON Web Algorithms [JWA].
                    auto x_bytes = TRY(public_key.x_bytes());
                    jwk.x = TRY(encode_base64url(x_bytes, AK::OmitPadding::Yes));

                    // 3. Set the y attribute of jwk according to the definition in Section 6.2.1.3 of JSON Web Algorithms [JWA].
                    auto y_bytes = TRY(public_key.y_bytes());
                    jwk.y = TRY(encode_base64url(y_bytes, AK::OmitPadding::Yes));

                    return {};
                },
                [&](::Crypto::PK::ECPrivateKey<> const& private_key) -> ErrorOr<void> {
                    Variant<Empty, ::Crypto::Curves::SECP256r1, ::Crypto::Curves::SECP384r1, ::Crypto::Curves::SECP521r1> curve;
                    if (algorithm.named_curve() == "P-256"sv)
                        curve = ::Crypto::Curves::SECP256r1 {};
                    else if (algorithm.named_curve() == "P-384"sv)
                        curve = ::Crypto::Curves::SECP384r1 {};
                    else if (algorithm.named_curve() == "P-521"sv)
                        curve = ::Crypto::Curves::SECP521r1 {};
                    else
                        VERIFY_NOT_REACHED();

                    auto maybe_public_key = curve.visit(
                        [](Empty const&) -> ErrorOr<::Crypto::Curves::SECPxxxr1Point> { return Error::from_string_literal("noop error"); },
                        [&](auto instance) { return instance.generate_public_key(private_key.d()); });

                    auto public_key = TRY(maybe_public_key);
                    auto x_bytes = TRY(public_key.x_bytes());
                    auto y_bytes = TRY(public_key.y_bytes());

                    // 2. Set the x attribute of jwk according to the definition in Section 6.2.1.2 of JSON Web Algorithms [JWA].
                    jwk.x = TRY(encode_base64url(x_bytes, AK::OmitPadding::Yes));

                    // 3. Set the y attribute of jwk according to the definition in Section 6.2.1.3 of JSON Web Algorithms [JWA].
                    jwk.y = TRY(encode_base64url(y_bytes, AK::OmitPadding::Yes));

                    return {};
                },
                [](auto) -> ErrorOr<void> {
                    VERIFY_NOT_REACHED();
                });

            if (maybe_error.is_error())
                return WebIDL::DataError::create(m_realm, "Failed to encode public key"_string);

            // 4. If the [[type]] internal slot of key is "private"
            if (key->type() == Bindings::KeyType::Private) {
                auto maybe_error = handle.visit(
                    [&](::Crypto::PK::ECPrivateKey<> const& private_key) -> ErrorOr<void> {
                        // Set the d attribute of jwk according to the definition in Section 6.2.2.1 of JSON Web Algorithms [JWA].
                        auto d_bytes = TRY(private_key.d_bytes());
                        jwk.d = TRY(encode_base64url(d_bytes, AK::OmitPadding::Yes));

                        return {};
                    },
                    [](auto) -> ErrorOr<void> {
                        VERIFY_NOT_REACHED();
                    });

                if (maybe_error.is_error())
                    return WebIDL::DataError::create(m_realm, "Failed to encode private key"_string);
            }
        } else {
            // 1. Perform any key export steps defined by other applicable specifications,
            //    passing format and the namedCurve attribute of the [[algorithm]] internal slot
            //    of key and obtaining namedCurve and a new value of jwk.
            // TODO: support 'applicable specifications'

            // 2. Set the crv attribute of jwk to namedCurve.
            jwk.crv = algorithm.named_curve();
        }

        // 4. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages())
            jwk.key_ops->append(idl_enum_to_string(usage));

        // 5. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 6. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        result = TRY(jwk.to_object(m_realm));
    }

    // 3. If format is "raw":
    else if (format == Bindings::KeyFormat::Raw) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a public key"_string);

        ByteBuffer data;

        // 2. If the namedCurve attribute of the [[algorithm]] internal slot of key is "P-256", "P-384" or "P-521":
        auto& algorithm = static_cast<EcKeyAlgorithm const&>(*key->algorithm());
        if (algorithm.named_curve().is_one_of("P-256"sv, "P-384"sv, "P-521"sv)) {
            // Let data be the octet string that represents the Elliptic Curve public key represented by the [[handle]] internal slot
            // of key according to the encoding rules specified in Section 2.3.3 of [SEC1] and using the uncompressed form.
            auto maybe_data = handle.visit(
                [](::Crypto::PK::ECPublicKey<> const& public_key) -> ErrorOr<ByteBuffer> {
                    return public_key.to_uncompressed();
                },
                [](auto) -> ErrorOr<ByteBuffer> {
                    VERIFY_NOT_REACHED();
                });

            if (maybe_data.is_error())
                return WebIDL::DataError::create(m_realm, "Failed to encode public key"_string);

            data = maybe_data.release_value();
        } else {
            // Perform any key export steps defined by other applicable specifications, passing format and
            // the namedCurve attribute of the [[algorithm]] internal slot of key and obtaining namedCurve and data.
            // TODO: support 'applicable specifications'
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // 3. Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(m_realm, move(data));
    }

    // 4. Return result.
    return GC::Ref { *result };
}

// https://wicg.github.io/webcrypto-secure-curves/#ed25519-operations
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> ED25519::generate_key([[maybe_unused]] AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains a value which is not one of "sign" or "verify", then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Sign && usage != Bindings::KeyUsage::Verify) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    // 2. Generate an Ed25519 key pair, as defined in [RFC8032], section 5.1.5.
    ::Crypto::Curves::Ed25519 curve;
    auto maybe_private_key = curve.generate_private_key();
    if (maybe_private_key.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to generate private key"_string);
    auto private_key_data = maybe_private_key.release_value();

    auto maybe_public_key = curve.generate_public_key(private_key_data);
    if (maybe_public_key.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to generate public key"_string);
    auto public_key_data = maybe_public_key.release_value();

    // 3. Let algorithm be a new KeyAlgorithm object.
    auto algorithm = KeyAlgorithm::create(m_realm);

    // 4. Set the name attribute of algorithm to "Ed25519".
    algorithm->set_name("Ed25519"_string);

    // 5. Let publicKey be a new CryptoKey associated with the relevant global object of this [HTML],
    // and representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key_data });

    // 6. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(Bindings::KeyType::Public);

    // 7. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(algorithm);

    // 8. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 9. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
    public_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Verify } }));

    // 10. Let privateKey be a new CryptoKey associated with the relevant global object of this [HTML],
    // and representing the private key of the generated key pair.
    auto private_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key_data });

    // 11. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(Bindings::KeyType::Private);

    // 12. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(algorithm);

    // 13. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 14. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
    private_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Sign } }));

    // 15. Let result be a new CryptoKeyPair dictionary.
    // 16. Set the publicKey attribute of result to be publicKey.
    // 17. Set the privateKey attribute of result to be privateKey.
    // 18. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
    return Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>> { CryptoKeyPair::create(m_realm, public_key, private_key) };
}

// https://wicg.github.io/webcrypto-secure-curves/#ed25519-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> ED25519::import_key(
    [[maybe_unused]] Web::Crypto::AlgorithmParams const& params,
    Bindings::KeyFormat format,
    CryptoKey::InternalKeyData key_data,
    bool extractable,
    Vector<Bindings::KeyUsage> const& usages)
{
    GC::Ptr<CryptoKey> key = nullptr;

    // 1. Let keyData be the key data to be imported.

    // 2. If format is "spki":
    if (format == Bindings::KeyFormat::Spki) {
        // 1. If usages contains a value which is not "verify" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Verify) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
        //    is not equal to the id-Ed25519 object identifier defined in [RFC8410], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::ed25519_oid)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm identifier"_string);

        // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
        if (spki.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

        // 6. Let publicKey be the Ed25519 public key identified by the subjectPublicKey field of spki.
        auto const& public_key = spki.raw_key;

        // 7. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
        //    and that represents publicKey.
        key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

        // 8. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);

        // 9. Let algorithm be a new KeyAlgorithm.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 10. Set the name attribute of algorithm to "Ed25519".
        algorithm->set_name("Ed25519"_string);

        // 11. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "pkcs8":
    else if (format == Bindings::KeyFormat::Pkcs8) {
        // 1. If usages contains a value which is not "sign" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Sign) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurs while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field
        //    of privateKeyInfo is not equal to the id-Ed25519 object identifier defined in [RFC8410], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::ed25519_oid)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm identifier"_string);

        // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo is present,
        //    then throw a DataError.
        if (private_key_info.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

        // 6. Let curvePrivateKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 CurvePrivateKey structure
        //    specified in Section 7 of [RFC8410], and exactData set to true.
        // 7. If an error occurred while parsing, then throw a DataError.
        auto curve_private_key = TRY(parse_an_ASN1_structure<StringView>(m_realm, private_key_info.raw_key, true));
        auto curve_private_key_bytes = TRY_OR_THROW_OOM(m_realm->vm(), ByteBuffer::copy(curve_private_key.bytes()));

        // 8. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
        //    and that represents the Ed25519 private key identified by curvePrivateKey.
        key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { curve_private_key_bytes });

        // 9. Set the [[type]] internal slot of key to "private"
        key->set_type(Bindings::KeyType::Private);

        // 10. Let algorithm be a new KeyAlgorithm.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 11. Set the name attribute of algorithm to "Ed25519".
        algorithm->set_name("Ed25519"_string);

        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "jwk":
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
        //    Otherwise: Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "keyData is not a JsonWebKey dictionary"_string);
        auto& jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the d field is present and usages contains a value which is not "sign",
        //    or, if the d field is not present and usages contains a value which is not "verify" then throw a SyntaxError.
        if (jwk.d.has_value()) {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Sign) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
                }
            }
        } else {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Verify) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
                }
            }
        }

        // 3. If the kty field of jwk is not "OKP", then throw a DataError.
        if (jwk.kty != "OKP"sv)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 4. If the crv field of jwk is not "Ed25519", then throw a DataError.
        if (jwk.crv != "Ed25519"sv)
            return WebIDL::DataError::create(m_realm, "Invalid curve"_string);

        // 5. If usages is non-empty and the use field of jwk is present and is not "sig", then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && jwk.use.value() != "sig")
            return WebIDL::DataError::create(m_realm, "Invalid key usage"_string);

        // 6. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
        //    or it does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(m_realm, jwk, usages));

        // 7. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !jwk.ext.value() && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid extractable"_string);

        // 8. If the d field is present:
        if (jwk.d.has_value()) {
            // 1. If jwk does not meet the requirements of the JWK private key format described in Section 2 of [RFC8037],
            //    then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "Ed25519"sv)
                return WebIDL::DataError::create(m_realm, "Invalid curve"_string);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create(m_realm, "Missing x field"_string);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (!jwk.d.has_value())
                return WebIDL::DataError::create(m_realm, "Present d field"_string);

            // 2. Let key be a new CryptoKey object that represents the Ed25519 private key identified by interpreting jwk according to Section 2 of [RFC8037].
            auto private_key_base_64 = jwk.d.value();
            auto private_key_or_error = decode_base64url(private_key_base_64);
            if (private_key_or_error.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
            }
            auto private_key = private_key_or_error.release_value();
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key });

            // 3. Set the [[type]] internal slot of Key to "private".
            key->set_type(Bindings::KeyType::Private);
        }
        // Otherwise:
        else {
            // 1. If jwk does not meet the requirements of the JWK public key format described in Section 2 of [RFC8037], then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "Ed25519"sv)
                return WebIDL::DataError::create(m_realm, "Invalid curve"_string);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create(m_realm, "Missing x field"_string);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (jwk.d.has_value())
                return WebIDL::DataError::create(m_realm, "Present d field"_string);

            // 2. Let key be a new CryptoKey object that represents the Ed25519 public key identified by interpreting jwk according to Section 2 of [RFC8037].
            auto public_key_base_64 = jwk.x.value();
            auto public_key_or_error = decode_base64url(public_key_base_64);
            if (public_key_or_error.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
            }
            auto public_key = public_key_or_error.release_value();
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

            // 3. Set the [[type]] internal slot of Key to "public".
            key->set_type(Bindings::KeyType::Public);
        }

        // 9. Let algorithm be a new instance of a KeyAlgorithm object.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 10. Set the name attribute of algorithm to "Ed25519".
        algorithm->set_name("Ed25519"_string);

        // 11. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "raw":
    else if (format == Bindings::KeyFormat::Raw) {
        // 1. If usages contains a value which is not "verify" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Verify) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        // 2. Let algorithm be a new KeyAlgorithm object.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 3. Set the name attribute of algorithm to "Ed25519".
        algorithm->set_name("Ed25519"_string);

        // 4. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and representing the key data provided in keyData.
        key = CryptoKey::create(m_realm, key_data);

        // 5. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);

        // 6. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. Otherwise:
    else {
        // throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Invalid key format"_string);
    }

    return GC::Ref { *key };
}

WebIDL::ExceptionOr<GC::Ref<JS::Object>> ED25519::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    auto& vm = m_realm->vm();

    // 1. Let key be the CryptoKey to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& key_data = key->handle().get<ByteBuffer>();

    // 3. If format is "spki":
    if (format == Bindings::KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a public key"_string);

        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //      * Set the algorithm object identifier to the id-Ed25519 OID defined in [RFC8410].
        //    * Set the subjectPublicKey field to keyData.
        auto ed25519_oid = ::Crypto::ASN1::ed25519_oid;
        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_subject_public_key_info(key_data, ed25519_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return JS::ArrayBuffer::create(m_realm, move(data));
    }

    // 3. If format is "pkcs8":
    if (format == Bindings::KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Private)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a private key"_string);

        // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
        //    * Set the version field to 0.
        //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        //      * Set the algorithm object identifier to the id-Ed25519 OID defined in [RFC8410].
        //    * Set the privateKey field to the result of DER-encoding a CurvePrivateKey ASN.1 type,
        //      as defined in Section 7 of [RFC8410], that represents the Ed25519 private key
        //      represented by the [[handle]] internal slot of key
        ::Crypto::ASN1::Encoder encoder;
        TRY_OR_THROW_OOM(vm, encoder.write(key_data.bytes()));

        auto ed25519_oid = ::Crypto::ASN1::ed25519_oid;
        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_private_key_info(encoder.finish(), ed25519_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return JS::ArrayBuffer::create(m_realm, move(data));
    }

    // 2. If format is "jwk":
    if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk;

        // 2. Set the kty attribute of jwk to "OKP".
        jwk.kty = "OKP"_string;

        // 3. Set the crv attribute of jwk to "Ed25519".
        jwk.crv = "Ed25519"_string;

        // 4. Set the x attribute of jwk according to the definition in Section 2 of [RFC8037].
        if (key->type() == Bindings::KeyType::Public) {
            jwk.x = TRY_OR_THROW_OOM(vm, encode_base64url(key_data, AK::OmitPadding::Yes));
        } else {
            // The "x" parameter of the "epk" field is set as follows:
            // Apply the appropriate ECDH function to the ephemeral private key (as scalar input)
            // and the standard base point (as u-coordinate input).
            // The base64url encoding of the output is the value for the "x" parameter of the "epk" field.
            ::Crypto::Curves::Ed25519 curve;
            auto public_key = TRY_OR_THROW_OOM(vm, curve.generate_public_key(key_data));
            jwk.x = TRY_OR_THROW_OOM(vm, encode_base64url(public_key, AK::OmitPadding::Yes));
        }

        // 5. If the [[type]] internal slot of key is "private"
        if (key->type() == Bindings::KeyType::Private) {
            // 1. Set the d attribute of jwk according to the definition in Section 2 of [RFC8037].
            jwk.d = TRY_OR_THROW_OOM(vm, encode_base64url(key_data, AK::OmitPadding::Yes));
        }

        // 6. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages())
            jwk.key_ops->append(Bindings::idl_enum_to_string(usage));

        // 7. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 8. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        return TRY(jwk.to_object(m_realm));
    }

    // 2. If format is "raw":
    if (format == Bindings::KeyFormat::Raw) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a public key"_string);

        // 2. Let data be an octet string representing the Ed25519 public key represented by the [[handle]] internal slot of key.
        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return JS::ArrayBuffer::create(m_realm, key_data);
    }

    // 2. Otherwise:
    //    throw a NotSupportedError.
    return WebIDL::NotSupportedError::create(m_realm, "Invalid key format"_string);
}

WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> ED25519::sign([[maybe_unused]] AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& message)
{
    auto& realm = *m_realm;
    auto& vm = realm.vm();

    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Private)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a private key"_string);

    // 2. Perform the Ed25519 signing process, as specified in [RFC8032], Section 5.1.6,
    // with message as M, using the Ed25519 private key associated with key.
    auto private_key = key->handle().get<ByteBuffer>();

    ::Crypto::Curves::Ed25519 curve;
    auto maybe_public_key = curve.generate_public_key(private_key);
    if (maybe_public_key.is_error())
        return WebIDL::OperationError::create(realm, "Failed to generate public key"_string);
    auto public_key = maybe_public_key.release_value();

    auto maybe_signature = curve.sign(private_key, message);
    if (maybe_signature.is_error())
        return WebIDL::OperationError::create(realm, "Failed to sign message"_string);
    auto signature = maybe_signature.release_value();

    // 3. Return a new ArrayBuffer associated with the relevant global object of this [HTML],
    // and containing the bytes of the signature resulting from performing the Ed25519 signing process.
    auto result = TRY_OR_THROW_OOM(vm, ByteBuffer::copy(signature));
    return JS::ArrayBuffer::create(realm, move(result));
}

WebIDL::ExceptionOr<JS::Value> ED25519::verify([[maybe_unused]] AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& signature, ByteBuffer const& message)
{
    auto& realm = *m_realm;

    // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Public)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a public key"_string);

    // NOTE: this is checked by ED25519::verify()
    // 2. If the key data of key represents an invalid point or a small-order element on the Elliptic Curve of Ed25519, return false.
    // 3. If the point R, encoded in the first half of signature, represents an invalid point or a small-order element on the Elliptic Curve of Ed25519, return false.

    // 4. Perform the Ed25519 verification steps, as specified in [RFC8032], Section 5.1.7,
    // using the cofactorless (unbatched) equation, [S]B = R + [k]A', on the signature,
    // with message as M, using the Ed25519 public key associated with key.

    auto public_key = key->handle().get<ByteBuffer>();

    // 9. Let result be a boolean with the value true if the signature is valid and the value false otherwise.
    ::Crypto::Curves::Ed25519 curve;
    auto maybe_verified = curve.verify(key->handle().get<ByteBuffer>(), signature, message);
    if (maybe_verified.is_error()) {
        auto error_message = MUST(String::from_utf8(maybe_verified.error().string_literal()));
        return WebIDL::OperationError::create(realm, error_message);
    }

    // 10. Return result.
    return maybe_verified.release_value();
}

// https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> ED448::generate_key([[maybe_unused]] AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains a value which is not one of "sign" or "verify", then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Sign && usage != Bindings::KeyUsage::Verify) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    // 2. Generate an Ed448 key pair, as defined in [RFC8032], section 5.1.5.
    ::Crypto::Curves::Ed448 curve;
    auto maybe_private_key = curve.generate_private_key();
    if (maybe_private_key.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to generate private key"_string);
    auto private_key_data = maybe_private_key.release_value();

    auto maybe_public_key = curve.generate_public_key(private_key_data);
    if (maybe_public_key.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to generate public key"_string);
    auto public_key_data = maybe_public_key.release_value();

    // 3. Let algorithm be a new KeyAlgorithm object.
    auto algorithm = KeyAlgorithm::create(m_realm);

    // 4. Set the name attribute of algorithm to "Ed448".
    algorithm->set_name("Ed448"_string);

    // 5. Let publicKey be a new CryptoKey associated with the relevant global object of this [HTML],
    // and representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key_data });

    // 6. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(Bindings::KeyType::Public);

    // 7. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(algorithm);

    // 8. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 9. Set the [[usages]] internal slot of publicKey to be the usage intersection of usages and [ "verify" ].
    public_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Verify } }));

    // 10. Let privateKey be a new CryptoKey associated with the relevant global object of this [HTML],
    // and representing the private key of the generated key pair.
    auto private_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key_data });

    // 11. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(Bindings::KeyType::Private);

    // 12. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(algorithm);

    // 13. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 14. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "sign" ].
    private_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Sign } }));

    // 15. Let result be a new CryptoKeyPair dictionary.
    // 16. Set the publicKey attribute of result to be publicKey.
    // 17. Set the privateKey attribute of result to be privateKey.
    // 18. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
    return Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>> { CryptoKeyPair::create(m_realm, public_key, private_key) };
}

// https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> ED448::import_key(
    [[maybe_unused]] Web::Crypto::AlgorithmParams const& params,
    Bindings::KeyFormat format,
    CryptoKey::InternalKeyData key_data,
    bool extractable,
    Vector<Bindings::KeyUsage> const& usages)
{
    GC::Ptr<CryptoKey> key = nullptr;

    // 1. Let keyData be the key data to be imported.

    // 2. If format is "spki":
    if (format == Bindings::KeyFormat::Spki) {
        // 1. If usages contains a value which is not "verify" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Verify) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
        //    is not equal to the id-Ed448 object identifier defined in [RFC8410], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::ed448_oid)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm identifier"_string);

        // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
        if (spki.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

        // 6. Let publicKey be the Ed448 public key identified by the subjectPublicKey field of spki.
        auto const& public_key = spki.raw_key;

        // 7. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
        //    and that represents publicKey.
        key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

        // 8. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);

        // 9. Let algorithm be a new KeyAlgorithm.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 10. Set the name attribute of algorithm to "Ed448".
        algorithm->set_name("Ed448"_string);

        // 11. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "pkcs8":
    else if (format == Bindings::KeyFormat::Pkcs8) {
        // 1. If usages contains a value which is not "sign" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Sign) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurs while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field
        //    of privateKeyInfo is not equal to the id-Ed448 object identifier defined in [RFC8410], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::ed448_oid)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm identifier"_string);

        // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo is present,
        //    then throw a DataError.
        if (private_key_info.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

        // 6. Let curvePrivateKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 CurvePrivateKey structure
        //    specified in Section 7 of [RFC8410], and exactData set to true.
        // 7. If an error occurred while parsing, then throw a DataError.
        auto curve_private_key = TRY(parse_an_ASN1_structure<StringView>(m_realm, private_key_info.raw_key, true));
        auto curve_private_key_bytes = TRY_OR_THROW_OOM(m_realm->vm(), ByteBuffer::copy(curve_private_key.bytes()));

        // 8. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
        //    and that represents the Ed448 private key identified by curvePrivateKey.
        key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { curve_private_key_bytes });

        // 9. Set the [[type]] internal slot of key to "private"
        key->set_type(Bindings::KeyType::Private);

        // 10. Let algorithm be a new KeyAlgorithm.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 11. Set the name attribute of algorithm to "Ed448".
        algorithm->set_name("Ed448"_string);

        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "jwk":
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
        //    Otherwise: Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "keyData is not a JsonWebKey dictionary"_string);
        auto& jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the d field is present and usages contains a value which is not "sign",
        //    or, if the d field is not present and usages contains a value which is not "verify" then throw a SyntaxError.
        if (jwk.d.has_value()) {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Sign) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
                }
            }
        } else {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Verify) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
                }
            }
        }

        // 3. If the kty field of jwk is not "OKP", then throw a DataError.
        if (jwk.kty != "OKP"sv)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 4. If the crv field of jwk is not "Ed448", then throw a DataError.
        if (jwk.crv != "Ed448"sv)
            return WebIDL::DataError::create(m_realm, "Invalid curve"_string);

        // 5. If usages is non-empty and the use field of jwk is present and is not "sig", then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && jwk.use.value() != "sig")
            return WebIDL::DataError::create(m_realm, "Invalid key usage"_string);

        // 6. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
        //    or it does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(m_realm, jwk, usages));

        // 7. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !jwk.ext.value() && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid extractable"_string);

        // 8. If the d field is present:
        if (jwk.d.has_value()) {
            // 1. If jwk does not meet the requirements of the JWK private key format described in Section 2 of [RFC8037],
            //    then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "Ed448"sv)
                return WebIDL::DataError::create(m_realm, "Invalid curve"_string);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create(m_realm, "Missing x field"_string);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (!jwk.d.has_value())
                return WebIDL::DataError::create(m_realm, "Present d field"_string);

            // 2. Let key be a new CryptoKey object that represents the Ed448 private key identified by interpreting jwk according to Section 2 of [RFC8037].
            auto private_key_base_64 = jwk.d.value();
            auto private_key_or_error = decode_base64url(private_key_base_64);
            if (private_key_or_error.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
            }
            auto private_key = private_key_or_error.release_value();
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key });

            // 3. Set the [[type]] internal slot of Key to "private".
            key->set_type(Bindings::KeyType::Private);
        }
        // Otherwise:
        else {
            // 1. If jwk does not meet the requirements of the JWK public key format described in Section 2 of [RFC8037], then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "Ed448"sv)
                return WebIDL::DataError::create(m_realm, "Invalid curve"_string);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create(m_realm, "Missing x field"_string);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (jwk.d.has_value())
                return WebIDL::DataError::create(m_realm, "Present d field"_string);

            // 2. Let key be a new CryptoKey object that represents the Ed448 public key identified by interpreting jwk according to Section 2 of [RFC8037].
            auto public_key_base_64 = jwk.x.value();
            auto public_key_or_error = decode_base64url(public_key_base_64);
            if (public_key_or_error.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
            }
            auto public_key = public_key_or_error.release_value();
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

            // 3. Set the [[type]] internal slot of Key to "public".
            key->set_type(Bindings::KeyType::Public);
        }

        // 9. Let algorithm be a new instance of a KeyAlgorithm object.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 10. Set the name attribute of algorithm to "Ed448".
        algorithm->set_name("Ed448"_string);

        // 11. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "raw":
    else if (format == Bindings::KeyFormat::Raw) {
        // 1. If usages contains a value which is not "verify" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Verify) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        // 2. Let algorithm be a new KeyAlgorithm object.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 3. Set the name attribute of algorithm to "Ed448".
        algorithm->set_name("Ed448"_string);

        // 4. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and representing the key data provided in keyData.
        key = CryptoKey::create(m_realm, key_data);

        // 5. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);

        // 6. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. Otherwise:
    else {
        // throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Invalid key format"_string);
    }

    return GC::Ref { *key };
}

// https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
WebIDL::ExceptionOr<GC::Ref<JS::Object>> ED448::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    auto& vm = m_realm->vm();

    // 1. Let key be the CryptoKey to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& key_data = key->handle().get<ByteBuffer>();

    // 3. If format is "spki":
    if (format == Bindings::KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a public key"_string);

        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //      * Set the algorithm object identifier to the id-Ed448 OID defined in [RFC8410].
        //    * Set the subjectPublicKey field to keyData.
        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_subject_public_key_info(key_data, ::Crypto::ASN1::ed448_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return JS::ArrayBuffer::create(m_realm, move(data));
    }

    // 3. If format is "pkcs8":
    if (format == Bindings::KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Private)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a private key"_string);

        // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
        //    * Set the version field to 0.
        //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        //      * Set the algorithm object identifier to the id-Ed448 OID defined in [RFC8410].
        //    * Set the privateKey field to the result of DER-encoding a CurvePrivateKey ASN.1 type,
        //      as defined in Section 7 of [RFC8410], that represents the Ed448 private key
        //      represented by the [[handle]] internal slot of key
        ::Crypto::ASN1::Encoder encoder;
        TRY_OR_THROW_OOM(vm, encoder.write(key_data.bytes()));

        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_private_key_info(encoder.finish(), ::Crypto::ASN1::ed448_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return JS::ArrayBuffer::create(m_realm, move(data));
    }

    // 2. If format is "jwk":
    if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk;

        // 2. Set the kty attribute of jwk to "OKP".
        jwk.kty = "OKP"_string;

        // 3. Set the crv attribute of jwk to "Ed448".
        jwk.crv = "Ed448"_string;

        // 4. Set the x attribute of jwk according to the definition in Section 2 of [RFC8037].
        if (key->type() == Bindings::KeyType::Public) {
            jwk.x = TRY_OR_THROW_OOM(vm, encode_base64url(key_data, AK::OmitPadding::Yes));
        } else {
            // The "x" parameter of the "epk" field is set as follows:
            // Apply the appropriate ECDH function to the ephemeral private key (as scalar input)
            // and the standard base point (as u-coordinate input).
            // The base64url encoding of the output is the value for the "x" parameter of the "epk" field.
            ::Crypto::Curves::Ed448 curve;
            auto public_key = TRY_OR_THROW_OOM(vm, curve.generate_public_key(key_data));
            jwk.x = TRY_OR_THROW_OOM(vm, encode_base64url(public_key, AK::OmitPadding::Yes));
        }

        // 5. If the [[type]] internal slot of key is "private"
        if (key->type() == Bindings::KeyType::Private) {
            // 1. Set the d attribute of jwk according to the definition in Section 2 of [RFC8037].
            jwk.d = TRY_OR_THROW_OOM(vm, encode_base64url(key_data, AK::OmitPadding::Yes));
        }

        // 6. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages())
            jwk.key_ops->append(Bindings::idl_enum_to_string(usage));

        // 7. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 8. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        return TRY(jwk.to_object(m_realm));
    }

    // 2. If format is "raw":
    if (format == Bindings::KeyFormat::Raw) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a public key"_string);

        // 2. Let data be an octet string representing the Ed448 public key represented by the [[handle]] internal slot of key.
        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return JS::ArrayBuffer::create(m_realm, key_data);
    }

    // 2. Otherwise:
    //    throw a NotSupportedError.
    return WebIDL::NotSupportedError::create(m_realm, "Invalid key format"_string);
}

// https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> ED448::sign(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& message)
{
    auto& realm = *m_realm;

    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Private)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a private key"_string);

    // 2. Let context be the contents of the context member of normalizedAlgorithm
    //    or the empty octet string if the context member of normalizedAlgorithm is not present.
    auto const& algorithm = static_cast<Ed448Params const&>(params);
    auto context = algorithm.context.value_or(ByteBuffer());

    // 3. If context has a length greater than 255 bytes, then throw an OperationError.
    if (context.size() > 255)
        return WebIDL::OperationError::create(realm, "Context is too long"_string);

    // 4. Perform the Ed448 signing process, as specified in [RFC8032], Section 5.2.6,
    //    with message as M and context as C, using the Ed448 private key associated with key.
    ::Crypto::Curves::Ed448 curve;
    auto maybe_signature = curve.sign(key->handle().get<ByteBuffer>(), message, context);
    if (maybe_signature.is_error()) {
        return WebIDL::OperationError::create(realm, "Failed to sign message"_string);
    }

    // 5. Return a new ArrayBuffer associated with the relevant global object of this [HTML],
    //    and containing the bytes of the signature resulting from performing the Ed448 signing process.
    return JS::ArrayBuffer::create(m_realm, maybe_signature.release_value());
}

// https://wicg.github.io/webcrypto-secure-curves/#ed448-operations
WebIDL::ExceptionOr<JS::Value> ED448::verify(AlgorithmParams const& params, GC::Ref<CryptoKey> key, ByteBuffer const& signature, ByteBuffer const& message)
{
    auto& realm = *m_realm;

    // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Public)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a public key"_string);

    // 2. Let context be the contents of the context member of normalizedAlgorithm
    //    or the empty octet string if the context member of normalizedAlgorithm is not present.
    auto const& algorithm = static_cast<Ed448Params const&>(params);
    auto context = algorithm.context.value_or(ByteBuffer());

    // 3. If context has a length greater than 255 bytes, then throw an OperationError.
    if (context.size() > 255)
        return WebIDL::OperationError::create(realm, "Context is too long"_string);

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
    auto maybe_verified = curve.verify(key->handle().get<ByteBuffer>(), signature, message, context);
    if (maybe_verified.is_error()) {
        auto error_message = MUST(String::from_utf8(maybe_verified.error().string_literal()));
        return WebIDL::OperationError::create(realm, error_message);
    }

    // 7. Let result be a boolean with the value true if the signature is valid
    //    and the value false otherwise.
    // 8. Return result.
    return maybe_verified.release_value();
}

// https://w3c.github.io/webcrypto/#hkdf-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> HKDF::derive_bits(AlgorithmParams const& params, GC::Ref<CryptoKey> key, Optional<u32> length_optional)
{
    auto& realm = *m_realm;
    auto const& normalized_algorithm = static_cast<HKDFParams const&>(params);

    // 1. If length is null or is not a multiple of 8, then throw an OperationError.
    if (!length_optional.has_value() || *length_optional % 8 != 0)
        return WebIDL::OperationError::create(realm, "Length must be specified and divisible by 8"_string);

    // 2. Let keyDerivationKey be the secret represented by [[handle]] internal slot of key as the message.
    auto key_derivation_key = key->handle().get<ByteBuffer>();

    // 3. Let result be the result of performing the HKDF extract and then the HKDF expand step described in Section 2 of [RFC5869] using:
    //    * the hash member of normalizedAlgorithm as Hash,
    //    * keyDerivationKey as the input keying material, IKM,
    //    * the contents of the salt member of normalizedAlgorithm as salt,
    //    * the contents of the info member of normalizedAlgorithm as info,
    //    * length divided by 8 as the value of L,
    // Note: Although HKDF technically supports absent salt (treating it as hashLen many NUL bytes),
    // all major browsers instead raise a TypeError, for example:
    //     "Failed to execute 'deriveBits' on 'SubtleCrypto': HkdfParams: salt: Not a BufferSource"
    // Because we are forced by neither peer pressure nor the spec, we don't support it either.

    // Note: Check for zero length early because our implementation doesn't support it.
    if (*length_optional == 0) {
        return TRY(JS::ArrayBuffer::create(realm, static_cast<size_t>(0)));
    }

    auto const& hash_algorithm = TRY(normalized_algorithm.hash.name(realm.vm()));
    auto hash_kind = TRY([&] -> WebIDL::ExceptionOr<::Crypto::Hash::HashKind> {
        if (hash_algorithm == "SHA-1")
            return ::Crypto::Hash::HashKind::SHA1;
        if (hash_algorithm == "SHA-256")
            return ::Crypto::Hash::HashKind::SHA256;
        if (hash_algorithm == "SHA-384")
            return ::Crypto::Hash::HashKind::SHA384;
        if (hash_algorithm == "SHA-512")
            return ::Crypto::Hash::HashKind::SHA512;
        return WebIDL::NotSupportedError::create(m_realm, MUST(String::formatted("Invalid hash function '{}'", hash_algorithm)));
    }());

    ::Crypto::Hash::HKDF hkdf(hash_kind);
    auto maybe_result = hkdf.derive_key(Optional<ReadonlyBytes>(normalized_algorithm.salt), key_derivation_key, normalized_algorithm.info, *length_optional / 8);

    // 4. If the key derivation operation fails, then throw an OperationError.
    if (maybe_result.is_error())
        return WebIDL::OperationError::create(realm, "Failed to derive key"_string);

    // 5. Return result
    return JS::ArrayBuffer::create(realm, maybe_result.release_value());
}

WebIDL::ExceptionOr<JS::Value> HKDF::get_key_length(AlgorithmParams const&)
{
    // 1. Return null.
    return JS::js_null();
}

// https://w3c.github.io/webcrypto/#pbkdf2-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> PBKDF2::derive_bits(AlgorithmParams const& params, GC::Ref<CryptoKey> key, Optional<u32> length_optional)
{
    auto& realm = *m_realm;
    auto const& normalized_algorithm = static_cast<PBKDF2Params const&>(params);

    // 1. If length is null or is not a multiple of 8, then throw an OperationError.
    if (!length_optional.has_value() || *length_optional % 8 != 0)
        return WebIDL::OperationError::create(realm, "Length must be greater than 0 and divisible by 8"_string);

    // 2. If the iterations member of normalizedAlgorithm is zero, then throw an OperationError.
    if (normalized_algorithm.iterations == 0)
        return WebIDL::OperationError::create(realm, "Iterations must be greater than 0"_string);

    // 3. Let prf be the MAC Generation function described in Section 4 of [FIPS-198-1] using the hash function described by the hash member of normalizedAlgorithm.
    auto const& hash_algorithm = TRY(normalized_algorithm.hash.name(realm.vm()));

    // 4. Let result be the result of performing the PBKDF2 operation defined in Section 5.2 of [RFC8018]
    // using prf as the pseudo-random function, PRF,
    // the password represented by [[handle]] internal slot of key as the password, P,
    // the contents of the salt attribute of normalizedAlgorithm as the salt, S,
    // the value of the iterations attribute of normalizedAlgorithm as the iteration count, c,
    // and length divided by 8 as the intended key length, dkLen.
    auto password = key->handle().get<ByteBuffer>();
    auto salt = normalized_algorithm.salt;
    auto iterations = normalized_algorithm.iterations;
    auto derived_key_length_bytes = *length_optional / 8;

    auto hash_kind = TRY([&] -> WebIDL::ExceptionOr<::Crypto::Hash::HashKind> {
        if (hash_algorithm == "SHA-1")
            return ::Crypto::Hash::HashKind::SHA1;
        if (hash_algorithm == "SHA-256")
            return ::Crypto::Hash::HashKind::SHA256;
        if (hash_algorithm == "SHA-384")
            return ::Crypto::Hash::HashKind::SHA384;
        if (hash_algorithm == "SHA-512")
            return ::Crypto::Hash::HashKind::SHA512;
        return WebIDL::NotSupportedError::create(m_realm, MUST(String::formatted("Invalid hash function '{}'", hash_algorithm)));
    }());

    ::Crypto::Hash::PBKDF2 pbkdf2(hash_kind);
    auto maybe_result = pbkdf2.derive_key(password, salt, iterations, derived_key_length_bytes);

    // 5. If the key derivation operation fails, then throw an OperationError.
    if (maybe_result.is_error())
        return WebIDL::OperationError::create(realm, "Failed to derive key"_string);

    // 6. Return result
    return JS::ArrayBuffer::create(realm, maybe_result.release_value());
}

// https://w3c.github.io/webcrypto/#pbkdf2-operations
WebIDL::ExceptionOr<JS::Value> PBKDF2::get_key_length(AlgorithmParams const&)
{
    // 1. Return null.
    return JS::js_null();
}

// https://w3c.github.io/webcrypto/#pbkdf2-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> PBKDF2::import_key(AlgorithmParams const&, Bindings::KeyFormat format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If format is not "raw", throw a NotSupportedError
    if (format != Bindings::KeyFormat::Raw)
        return WebIDL::NotSupportedError::create(m_realm, "Only raw format is supported"_string);

    // 2. If usages contains a value that is not "deriveKey" or "deriveBits", then throw a SyntaxError.
    for (auto& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Derivekey && usage != Bindings::KeyUsage::Derivebits)
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
    }

    // 3. If extractable is not false, then throw a SyntaxError.
    if (extractable)
        return WebIDL::SyntaxError::create(m_realm, "extractable must be false"_string);

    // 4. Let key be a new CryptoKey representing keyData.
    auto key = CryptoKey::create(m_realm, move(key_data));

    // 5. Set the [[type]] internal slot of key to "secret".
    key->set_type(Bindings::KeyType::Secret);

    // 6. Let algorithm be a new KeyAlgorithm object.
    auto algorithm = KeyAlgorithm::create(m_realm);

    // 7. Set the name attribute of algorithm to "PBKDF2".
    algorithm->set_name("PBKDF2"_string);

    // 8. Set the [[algorithm]] internal slot of key to algorithm.
    key->set_algorithm(algorithm);

    // 9. Return key.
    return key;
}

// https://wicg.github.io/webcrypto-secure-curves/#x25519-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> X25519::derive_bits(AlgorithmParams const& params, GC::Ref<CryptoKey> key, Optional<u32> length_optional)
{
    auto& realm = *m_realm;
    auto const& normalized_algorithm = static_cast<EcdhKeyDeriveParams const&>(params);

    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Private)
        return WebIDL::InvalidAccessError::create(realm, "Key is not a private key"_string);

    // 2. Let publicKey be the public member of normalizedAlgorithm.
    auto& public_key = normalized_algorithm.public_key;

    // 3. If the [[type]] internal slot of publicKey is not "public", then throw an InvalidAccessError.
    if (public_key->type() != Bindings::KeyType::Public)
        return WebIDL::InvalidAccessError::create(realm, "Public key is not a public key"_string);

    // 4. If the name attribute of the [[algorithm]] internal slot of publicKey is not equal to
    //    the name property of the [[algorithm]] internal slot of key, then throw an InvalidAccessError.
    auto& internal_algorithm = static_cast<KeyAlgorithm const&>(*key->algorithm());
    auto& public_internal_algorithm = static_cast<KeyAlgorithm const&>(*public_key->algorithm());
    if (internal_algorithm.name() != public_internal_algorithm.name())
        return WebIDL::InvalidAccessError::create(realm, "Algorithm mismatch"_string);

    // 5. Let secret be the result of performing the X25519 function specified in [RFC7748] Section 5 with
    //    key as the X25519 private key k and
    //    the X25519 public key represented by the [[handle]] internal slot of publicKey as the X25519 public key u.
    auto private_key = key->handle().get<ByteBuffer>();
    auto public_key_data = public_key->handle().get<ByteBuffer>();

    ::Crypto::Curves::X25519 curve;
    auto maybe_secret = curve.compute_coordinate(private_key, public_key_data);
    if (maybe_secret.is_error())
        return WebIDL::OperationError::create(realm, "Failed to compute secret"_string);

    auto secret = maybe_secret.release_value();

    // 6. If secret is the all-zero value, then throw a OperationError.
    //    This check must be performed in constant-time, as per [RFC7748] Section 6.1.
    // NOTE: The check may be performed by ORing all the bytes together and checking whether the result is zero,
    //       as this eliminates standard side-channels in software implementations.
    auto or_bytes = 0;
    for (auto byte : secret.bytes()) {
        or_bytes |= byte;
    }

    if (or_bytes == 0)
        return WebIDL::OperationError::create(realm, "Secret is the all-zero value"_string);

    // 7. If length is null: Return secret
    if (!length_optional.has_value()) {
        auto result = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::copy(secret));
        return JS::ArrayBuffer::create(realm, move(result));
    }

    // Otherwise: If the length of secret in bits is less than length: throw an OperationError.
    auto length = length_optional.value();
    if (secret.size() * 8 < length)
        return WebIDL::OperationError::create(realm, "Secret is too short"_string);

    // Otherwise: Return an octet string containing the first length bits of secret.
    auto slice = TRY_OR_THROW_OOM(realm.vm(), secret.slice(0, ceil_div(length, 8)));
    if (length % 8 != 0) {
        // Zero out the last bits
        slice[slice.size() - 1] &= 0xFF << (8 - (length % 8));
    }

    return JS::ArrayBuffer::create(realm, move(slice));
}

WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> X25519::generate_key([[maybe_unused]] AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& key_usages)
{
    // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
    for (auto const& usage : key_usages) {
        if (usage != Bindings::KeyUsage::Derivekey && usage != Bindings::KeyUsage::Derivebits) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    // 2. Generate an X25519 key pair, with the private key being 32 random bytes,
    //    and the public key being X25519(a, 9), as defined in [RFC7748], section 6.1.
    ::Crypto::Curves::X25519 curve;
    auto maybe_private_key = curve.generate_private_key();
    if (maybe_private_key.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to generate private key"_string);

    auto private_key_data = maybe_private_key.release_value();

    auto maybe_public_key = curve.generate_public_key(private_key_data);
    if (maybe_public_key.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to generate public key"_string);

    auto public_key_data = maybe_public_key.release_value();

    // 3. Let algorithm be a new KeyAlgorithm object.
    auto algorithm = KeyAlgorithm::create(m_realm);

    // 4. Set the name attribute of algorithm to "X25519".
    algorithm->set_name("X25519"_string);

    // 5. Let publicKey be a new CryptoKey associated with the relevant global object of this [HTML],
    //    and representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key_data });

    // 6. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(Bindings::KeyType::Public);

    // 7. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(algorithm);

    // 8. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 9. Set the [[usages]] internal slot of publicKey to be the empty list.
    public_key->set_usages({});

    // 10. Let privateKey be a new CryptoKey associated with the relevant global object of this [HTML],
    //     and representing the private key of the generated key pair.
    auto private_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key_data });

    // 11. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(Bindings::KeyType::Private);

    // 12. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(algorithm);

    // 13. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 14. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "deriveKey", "deriveBits" ].
    private_key->set_usages(usage_intersection(key_usages, { { Bindings::KeyUsage::Derivekey, Bindings::KeyUsage::Derivebits } }));

    // 15. Let result be a new CryptoKeyPair dictionary.
    // 16. Set the publicKey attribute of result to be publicKey.
    // 17. Set the privateKey attribute of result to be privateKey.
    // 18. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
    return Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>> { CryptoKeyPair::create(m_realm, public_key, private_key) };
}

WebIDL::ExceptionOr<GC::Ref<CryptoKey>> X25519::import_key([[maybe_unused]] Web::Crypto::AlgorithmParams const& params, Bindings::KeyFormat key_format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& usages)
{
    // NOTE: This is a parameter to the function
    // 1. Let keyData be the key data to be imported.

    auto& vm = m_realm->vm();
    GC::Ptr<CryptoKey> key = nullptr;

    // 2. If format is "spki":
    if (key_format == Bindings::KeyFormat::Spki) {
        // 1. If usages is not empty then throw a SyntaxError.
        if (!usages.is_empty())
            return WebIDL::SyntaxError::create(m_realm, "Usages must be empty"_string);

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki
        //    is not equal to the id-X25519 object identifier defined in [RFC8410], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::x25519_oid)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
        if (spki.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

        // 6. Let publicKey be the X25519 public key identified by the subjectPublicKey field of spki.
        auto public_key = spki.raw_key;

        // 7. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents publicKey.
        key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

        // 8. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);

        // 9. Let algorithm be a new KeyAlgorithm.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 10. Set the name attribute of algorithm to "X25519".
        algorithm->set_name("X25519"_string);

        // 11. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "pkcs8":
    else if (key_format == Bindings::KeyFormat::Pkcs8) {
        // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Derivekey && usage != Bindings::KeyUsage::Derivebits) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo
        //    is not equal to the id-X25519 object identifier defined in [RFC8410], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::x25519_oid)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo is present, then throw a DataError.
        if (private_key_info.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

        // 6. Let curvePrivateKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the privateKey field of privateKeyInfo,
        //    structure as the ASN.1 CurvePrivateKey structure specified in Section 7 of [RFC8410], and
        //    exactData set to true.
        // 7. If an error occurred while parsing, then throw a DataError.
        auto curve_private_key = TRY(parse_an_ASN1_structure<StringView>(m_realm, private_key_info.raw_key, true));
        auto curve_private_key_bytes = TRY_OR_THROW_OOM(vm, ByteBuffer::copy(curve_private_key.bytes()));

        // 8. Let key be a new CryptoKey associated with the relevant global object of this [HTML],
        //    and that represents the X25519 private key identified by curvePrivateKey.
        key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { curve_private_key_bytes });

        // 9. Set the [[type]] internal slot of key to "private"
        key->set_type(Bindings::KeyType::Private);

        // 10. Let algorithm be a new KeyAlgorithm.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 11. Set the name attribute of algorithm to "X25519".
        algorithm->set_name("X25519"_string);

        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "jwk":
    else if (key_format == Bindings::KeyFormat::Jwk) {
        // 1. If keyData is a JsonWebKey dictionary: Let jwk equal keyData.
        //    Otherwise: Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "keyData is not a JsonWebKey dictionary"_string);
        auto& jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the d field is present and if usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        if (jwk.d.has_value() && !usages.is_empty()) {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Derivekey && usage != Bindings::KeyUsage::Derivebits) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
                }
            }
        }

        // 3. If the d field is not present and if usages is not empty then throw a SyntaxError.
        if (!jwk.d.has_value() && !usages.is_empty())
            return WebIDL::SyntaxError::create(m_realm, "Usages must be empty if d is missing"_string);

        // 4. If the kty field of jwk is not "OKP", then throw a DataError.
        if (jwk.kty != "OKP"sv)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 5. If the crv field of jwk is not "X25519", then throw a DataError.
        if (jwk.crv != "X25519"sv)
            return WebIDL::DataError::create(m_realm, "Invalid curve"_string);

        // 6. If usages is non-empty and the use field of jwk is present and is not equal to "enc" then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && jwk.use.value() != "enc"sv)
            return WebIDL::DataError::create(m_realm, "Invalid use"_string);

        // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
        //    or it does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(m_realm, jwk, usages));

        // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !jwk.ext.value() && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid extractable"_string);

        // 9. If the d field is present:
        if (jwk.d.has_value()) {
            // 1. If jwk does not meet the requirements of the JWK private key format described in Section 2 of [RFC8037], then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

            // // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "X25519"sv)
                return WebIDL::DataError::create(m_realm, "Invalid curve"_string);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create(m_realm, "Missing x field"_string);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (!jwk.d.has_value())
                return WebIDL::DataError::create(m_realm, "Missing d field"_string);

            // 2. Let key be a new CryptoKey object that represents the X25519 private key identified by interpreting jwk according to Section 2 of [RFC8037].
            auto private_key_base_64 = jwk.d.value();
            auto private_key_or_error = decode_base64url(private_key_base_64);
            if (private_key_or_error.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
            }
            auto private_key = private_key_or_error.release_value();
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key });

            // 3. Set the [[type]] internal slot of Key to "private".
            key->set_type(Bindings::KeyType::Private);
        }
        // 9. Otherwise:
        else {
            // 1. If jwk does not meet the requirements of the JWK public key format described in Section 2 of [RFC8037], then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "X25519"sv)
                return WebIDL::DataError::create(m_realm, "Invalid curve"_string);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create(m_realm, "Missing x field"_string);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (jwk.d.has_value())
                return WebIDL::DataError::create(m_realm, "Present d field"_string);

            // 2. Let key be a new CryptoKey object that represents the X25519 public key identified by interpreting jwk according to Section 2 of [RFC8037].
            auto public_key_base_64 = jwk.x.value();
            auto public_key_or_error = decode_base64url(public_key_base_64);
            if (public_key_or_error.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
            }
            auto public_key = public_key_or_error.release_value();
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

            // 3. Set the [[type]] internal slot of Key to "public".
            key->set_type(Bindings::KeyType::Public);
        }

        // 10. Let algorithm be a new instance of a KeyAlgorithm object.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 11. Set the name attribute of algorithm to "X25519".
        algorithm->set_name("X25519"_string);

        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. If format is "raw":
    else if (key_format == Bindings::KeyFormat::Raw) {
        // 1. If usages is not empty then throw a SyntaxError.
        if (!usages.is_empty())
            return WebIDL::SyntaxError::create(m_realm, "Usages must be empty"_string);

        // 2. Let algorithm be a new KeyAlgorithm object.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 3. Set the name attribute of algorithm to "X25519".
        algorithm->set_name("X25519"_string);

        // 4. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and representing the key data provided in keyData.
        key = CryptoKey::create(m_realm, key_data);

        // 5. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);

        // 6. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);
    }

    // 2. Otherwise: throw a NotSupportedError.
    else {
        return WebIDL::NotSupportedError::create(m_realm, "Invalid key format"_string);
    }

    // 3. Return key
    return GC::Ref { *key };
}

WebIDL::ExceptionOr<GC::Ref<JS::Object>> X25519::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    auto& vm = m_realm->vm();

    // NOTE: This is a parameter to the function
    // 1. Let key be the CryptoKey to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& handle = key->handle();

    GC::Ptr<JS::Object> result = nullptr;

    // 3. If format is "spki":
    if (format == Bindings::KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a public key"_string);

        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //    Set the algorithm object identifier to the id-X25519 OID defined in [RFC8410].
        //    Set the subjectPublicKey field to keyData.
        auto public_key = handle.get<ByteBuffer>();
        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_subject_public_key_info(public_key, ::Crypto::ASN1::x25519_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        result = JS::ArrayBuffer::create(m_realm, data);
    }

    // 3. If format is "pkcs8":
    else if (format == Bindings::KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Private)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a private key"_string);

        // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
        //    Set the version field to 0.
        //    Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        //    Set the algorithm object identifier to the id-X25519 OID defined in [RFC8410].
        //    Set the privateKey field to the result of DER-encoding a CurvePrivateKey ASN.1 type, as defined in Section 7 of [RFC8410],
        //    that represents the X25519 private key represented by the [[handle]] internal slot of key
        auto private_key = handle.get<ByteBuffer>();

        ::Crypto::ASN1::Encoder encoder;
        TRY_OR_THROW_OOM(vm, encoder.write(private_key));
        auto data = TRY_OR_THROW_OOM(vm, ::Crypto::PK::wrap_in_private_key_info(encoder.finish(), ::Crypto::ASN1::x25519_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        result = JS::ArrayBuffer::create(m_realm, data);
    }

    // 3. If format is "jwt":
    else if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionar1y.
        Bindings::JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to "OKP".
        jwk.kty = "OKP"_string;

        // 3. Set the crv attribute of jwk to "X25519".
        jwk.crv = "X25519"_string;

        // 4. Set the x attribute of jwk according to the definition in Section 2 of [RFC8037].
        if (key->type() == Bindings::KeyType::Public) {
            auto public_key = handle.get<ByteBuffer>();
            jwk.x = TRY_OR_THROW_OOM(vm, encode_base64url(public_key, AK::OmitPadding::Yes));
        } else {
            // The "x" parameter of the "epk" field is set as follows:
            // Apply the appropriate ECDH function to the ephemeral private key (as scalar input)
            // and the standard base point (as u-coordinate input).
            // The base64url encoding of the output is the value for the "x" parameter of the "epk" field.
            ::Crypto::Curves::X25519 curve;
            auto public_key = TRY_OR_THROW_OOM(vm, curve.generate_public_key(handle.get<ByteBuffer>()));
            jwk.x = TRY_OR_THROW_OOM(vm, encode_base64url(public_key, AK::OmitPadding::Yes));
        }

        // 5. If the [[type]] internal slot of key is "private"
        if (key->type() == Bindings::KeyType::Private) {
            // 1. Set the d attribute of jwk according to the definition in Section 2 of [RFC8037].
            auto private_key = handle.get<ByteBuffer>();
            jwk.d = TRY_OR_THROW_OOM(vm, encode_base64url(private_key, AK::OmitPadding::Yes));
        }

        // 6. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages())
            jwk.key_ops->append(Bindings::idl_enum_to_string(usage));

        // 7. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 8. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        result = TRY(jwk.to_object(m_realm));
    }

    // 3. If format is "raw":
    else if (format == Bindings::KeyFormat::Raw) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a public key"_string);

        // 2. Let data be an octet string representing the X25519 public key represented by the [[handle]] internal slot of key.
        auto public_key = handle.get<ByteBuffer>();

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        result = JS::ArrayBuffer::create(m_realm, public_key);
    }

    // 3. Otherwise:
    else {
        return WebIDL::NotSupportedError::create(m_realm, "Invalid key format"_string);
    }

    // 4. Return result.
    return GC::Ref { *result };
}

// https://wicg.github.io/webcrypto-secure-curves/#x448-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> X448::derive_bits(
    AlgorithmParams const& params,
    GC::Ref<CryptoKey> key,
    Optional<u32> length_optional)
{
    // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
    if (key->type() != Bindings::KeyType::Private)
        return WebIDL::InvalidAccessError::create(m_realm, "Key is not a private key"_string);

    // 2. Let publicKey be the public member of normalizedAlgorithm.
    auto& public_key = static_cast<EcdhKeyDeriveParams const&>(params).public_key;

    // 3. If the [[type]] internal slot of publicKey is not "public", then throw an InvalidAccessError.
    if (public_key->type() != Bindings::KeyType::Public)
        return WebIDL::InvalidAccessError::create(m_realm, "Public key is not a public key"_string);

    // 4. If the name attribute of the [[algorithm]] internal slot of publicKey is not equal to
    //    the name property of the [[algorithm]] internal slot of key, then throw an InvalidAccessError.
    auto& internal_algorithm = static_cast<KeyAlgorithm const&>(*key->algorithm());
    auto& public_internal_algorithm = static_cast<KeyAlgorithm const&>(*public_key->algorithm());
    if (internal_algorithm.name() != public_internal_algorithm.name())
        return WebIDL::InvalidAccessError::create(m_realm, "Algorithm mismatch"_string);

    // 5. Let secret be the result of performing the X448 function specified in [RFC7748] Section 5
    //    with key as the X448 private key k and the X448 public key represented by the [[handle]]
    //    internal slot of publicKey as the X448 public key u.
    auto private_key = key->handle().get<ByteBuffer>();
    auto public_key_data = public_key->handle().get<ByteBuffer>();

    ::Crypto::Curves::X448 curve;
    auto maybe_secret = curve.compute_coordinate(private_key, public_key_data);
    if (maybe_secret.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to compute secret"_string);

    auto secret = maybe_secret.release_value();

    // 6. If secret is the all-zero value, then throw a OperationError. This check must be performed in constant-time, as per [RFC7748] Section 6.2.
    // NOTE: The check may be performed by ORing all the bytes together and checking whether the result is zero,
    //       as this eliminates standard side-channels in software implementations.
    auto or_bytes = 0;
    for (auto byte : secret.bytes()) {
        or_bytes |= byte;
    }

    if (or_bytes == 0)
        return WebIDL::OperationError::create(m_realm, "Secret is the all-zero value"_string);

    // 7. If length is null: Return secret
    if (!length_optional.has_value()) {
        auto result = TRY_OR_THROW_OOM(m_realm->vm(), ByteBuffer::copy(secret));
        return JS::ArrayBuffer::create(m_realm, move(result));
    }

    // Otherwise: Return an octet string containing the first length bits of secret.
    auto length = length_optional.value();
    if (secret.size() * 8 < length)
        return WebIDL::OperationError::create(m_realm, "Secret is too short"_string);

    auto slice = TRY_OR_THROW_OOM(m_realm->vm(), secret.slice(0, ceil_div(length, 8)));
    if (length % 8 != 0) {
        // Zero out the last bits
        slice[slice.size() - 1] &= 0xFF << (8 - (length % 8));
    }

    return JS::ArrayBuffer::create(m_realm, move(slice));
}

// https://wicg.github.io/webcrypto-secure-curves/#x448-operations
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> X448::generate_key(
    AlgorithmParams const&,
    bool extractable,
    Vector<Bindings::KeyUsage> const& usages)
{
    // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
    for (auto const& usage : usages) {
        if (usage != Bindings::KeyUsage::Derivekey && usage != Bindings::KeyUsage::Derivebits) {
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
        }
    }

    // 2. Generate an X448 key pair, with the private key being 56 random bytes, and the public key being X448(a, 5), as defined in [RFC7748], section 6.2.
    ::Crypto::Curves::X448 curve;
    auto maybe_private_key = curve.generate_private_key();
    if (maybe_private_key.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to generate private key"_string);
    auto private_key_data = maybe_private_key.release_value();
    auto maybe_public_key = curve.generate_public_key(private_key_data);
    if (maybe_public_key.is_error())
        return WebIDL::OperationError::create(m_realm, "Failed to generate public key"_string);
    auto public_key_data = maybe_public_key.release_value();

    // 3. Let algorithm be a new KeyAlgorithm object.
    auto algorithm = KeyAlgorithm::create(m_realm);

    // 4. Set the name attribute of algorithm to "X448".
    algorithm->set_name("X448"_string);

    // 5. Let publicKey be a new CryptoKey associated with the relevant global object of this [HTML], and representing the public key of the generated key pair.
    auto public_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key_data });

    // 6. Set the [[type]] internal slot of publicKey to "public"
    public_key->set_type(Bindings::KeyType::Public);

    // 7. Set the [[algorithm]] internal slot of publicKey to algorithm.
    public_key->set_algorithm(algorithm);

    // 8. Set the [[extractable]] internal slot of publicKey to true.
    public_key->set_extractable(true);

    // 9. Set the [[usages]] internal slot of publicKey to be the empty list.
    public_key->set_usages({});

    // 10. Let privateKey be a new CryptoKey associated with the relevant global object of this [HTML], and representing the private key of the generated key pair.
    auto private_key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key_data });

    // 11. Set the [[type]] internal slot of privateKey to "private"
    private_key->set_type(Bindings::KeyType::Private);

    // 12. Set the [[algorithm]] internal slot of privateKey to algorithm.
    private_key->set_algorithm(algorithm);

    // 13. Set the [[extractable]] internal slot of privateKey to extractable.
    private_key->set_extractable(extractable);

    // 14. Set the [[usages]] internal slot of privateKey to be the usage intersection of usages and [ "deriveKey", "deriveBits" ].
    private_key->set_usages(usage_intersection(usages, { { Bindings::KeyUsage::Derivekey, Bindings::KeyUsage::Derivebits } }));

    // 15. Let result be a new CryptoKeyPair dictionary.
    // 16. Set the publicKey attribute of result to be publicKey.
    // 17. Set the privateKey attribute of result to be privateKey.
    auto result = CryptoKeyPair::create(m_realm, public_key, private_key);

    // 18. Return the result of converting result to an ECMAScript Object, as defined by [WebIDL].
    return Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>> { result };
}

// https://wicg.github.io/webcrypto-secure-curves/#x448-operations
WebIDL::ExceptionOr<GC::Ref<JS::Object>> X448::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    // 1. Let key be the CryptoKey to be exported.

    // 2. If the underlying cryptographic key material represented by the [[handle]] internal slot of key cannot be accessed, then throw an OperationError.
    // Note: In our impl this is always accessible
    auto const& key_data = key->handle().get<ByteBuffer>();

    // 3. If format is "spki":
    if (format == Bindings::KeyFormat::Spki) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a public key"_string);

        // 2. Let data be an instance of the subjectPublicKeyInfo ASN.1 structure defined in [RFC5280] with the following properties:
        //    * Set the algorithm field to an AlgorithmIdentifier ASN.1 type with the following properties:
        //      * Set the algorithm object identifier to the id-X448 OID defined in [RFC8410].
        //    * Set the subjectPublicKey field to keyData.
        auto x448_oid = ::Crypto::ASN1::x448_oid;
        auto data = TRY_OR_THROW_OOM(m_realm->vm(), ::Crypto::PK::wrap_in_subject_public_key_info(key_data, x448_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return JS::ArrayBuffer::create(m_realm, data);
    }

    // 3. If format is "pkcs8":
    if (format == Bindings::KeyFormat::Pkcs8) {
        // 1. If the [[type]] internal slot of key is not "private", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Private)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a private key"_string);

        // 2. Let data be an instance of the privateKeyInfo ASN.1 structure defined in [RFC5208] with the following properties:
        //    * Set the version field to 0.
        //    * Set the privateKeyAlgorithm field to a PrivateKeyAlgorithmIdentifier ASN.1 type with the following properties:
        //      * Set the algorithm object identifier to the id-X448 OID defined in [RFC8410].
        //    * Set the privateKey field to the result of DER-encoding a CurvePrivateKey ASN.1 type, as defined in Section 7 of [RFC8410], that represents the X448 private key represented by the [[handle]] internal slot of key
        ::Crypto::ASN1::Encoder encoder;
        TRY_OR_THROW_OOM(m_realm->vm(), encoder.write(key_data.bytes()));

        auto x448_oid = ::Crypto::ASN1::x448_oid;
        auto data = TRY_OR_THROW_OOM(m_realm->vm(), ::Crypto::PK::wrap_in_private_key_info(encoder.finish(), x448_oid));

        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return JS::ArrayBuffer::create(m_realm, data);
    }

    // 3. If format is "jwk":
    if (format == Bindings::KeyFormat::Jwk) {
        // 1. Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk = {};

        // 2. Set the kty attribute of jwk to "OKP".
        jwk.kty = "OKP"_string;

        // 3. Set the crv attribute of jwk to "X448".
        jwk.crv = "X448"_string;

        // 4. Set the x attribute of jwk according to the definition in Section 2 of [RFC8037].
        if (key->type() == Bindings::KeyType::Public) {
            jwk.x = TRY_OR_THROW_OOM(m_realm->vm(), encode_base64url(key_data, AK::OmitPadding::Yes));
        } else {
            ::Crypto::Curves::X448 curve;
            auto public_key = TRY_OR_THROW_OOM(m_realm->vm(), curve.generate_public_key(key_data));
            jwk.x = TRY_OR_THROW_OOM(m_realm->vm(), encode_base64url(public_key, AK::OmitPadding::Yes));
        }

        // 5. If the [[type]] internal slot of key is "private"
        if (key->type() == Bindings::KeyType::Private) {
            // 1. Set the d attribute of jwk according to the definition in Section 2 of [RFC8037].
            jwk.d = TRY_OR_THROW_OOM(m_realm->vm(), encode_base64url(key_data, AK::OmitPadding::Yes));
        }

        // 6. Set the key_ops attribute of jwk to the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages())
            jwk.key_ops->append(Bindings::idl_enum_to_string(usage));

        // 7. Set the ext attribute of jwk to the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // 8. Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        auto result = TRY(jwk.to_object(m_realm));
        return GC::Ref { *result };
    }

    // 3. If format is "raw":
    if (format == Bindings::KeyFormat::Raw) {
        // 1. If the [[type]] internal slot of key is not "public", then throw an InvalidAccessError.
        if (key->type() != Bindings::KeyType::Public)
            return WebIDL::InvalidAccessError::create(m_realm, "Key is not a public key"_string);

        // 2. Let data be an octet string representing the X448 public key represented by the [[handle]] internal slot of key.
        // 3. Let result be a new ArrayBuffer associated with the relevant global object of this [HTML], and containing data.
        return JS::ArrayBuffer::create(m_realm, key_data);
    }

    // 3. Otherwise:
    //    throw a NotSupportedError.
    return WebIDL::NotSupportedError::create(m_realm, "Invalid key format"_string);
}

// https://wicg.github.io/webcrypto-secure-curves/#x448-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> X448::import_key(
    AlgorithmParams const&,
    Bindings::KeyFormat format,
    CryptoKey::InternalKeyData key_data,
    bool extractable,
    Vector<Bindings::KeyUsage> const& usages)
{
    // 1. Let keyData be the key data to be imported.

    // 2. If format is "spki":
    if (format == Bindings::KeyFormat::Spki) {
        // 1. If usages is not empty then throw a SyntaxError.
        if (!usages.is_empty())
            return WebIDL::SyntaxError::create(m_realm, "Usages must be empty"_string);

        // 2. Let spki be the result of running the parse a subjectPublicKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto spki = TRY(parse_a_subject_public_key_info(m_realm, key_data.get<ByteBuffer>()));

        // 4. If the algorithm object identifier field of the algorithm AlgorithmIdentifier field of spki is not equal to the id-X448 object identifier defined in [RFC8410], then throw a DataError.
        if (spki.algorithm.identifier != ::Crypto::ASN1::x448_oid)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // 5. If the parameters field of the algorithm AlgorithmIdentifier field of spki is present, then throw a DataError.
        if (spki.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

        // 6. Let publicKey be the X448 public key identified by the subjectPublicKey field of spki.
        auto const& public_key = spki.raw_key;

        // 7. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents publicKey.
        auto key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

        // 8. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);

        // 9. Let algorithm be a new KeyAlgorithm.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 10. Set the name attribute of algorithm to "X448".
        algorithm->set_name("X448"_string);

        // 11. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);

        return key;
    }

    // 3. If format is "pkcs8":
    if (format == Bindings::KeyFormat::Pkcs8) {
        // 1. If usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        for (auto const& usage : usages) {
            if (usage != Bindings::KeyUsage::Derivekey && usage != Bindings::KeyUsage::Derivebits) {
                return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
            }
        }

        // 2. Let privateKeyInfo be the result of running the parse a privateKeyInfo algorithm over keyData.
        // 3. If an error occurred while parsing, then throw a DataError.
        auto private_key_info = TRY(parse_a_private_key_info(m_realm, key_data.get<ByteBuffer>()));
        auto private_key = private_key_info.raw_key;

        // 4. If the algorithm object identifier field of the privateKeyAlgorithm PrivateKeyAlgorithm field of privateKeyInfo is not equal to the id-X448 object identifier defined in [RFC8410], then throw a DataError.
        if (private_key_info.algorithm.identifier != ::Crypto::ASN1::x448_oid)
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);

        // 5. If the parameters field of the privateKeyAlgorithm PrivateKeyAlgorithmIdentifier field of privateKeyInfo is present, then throw a DataError.
        if (private_key_info.algorithm.ec_parameters.has_value())
            return WebIDL::DataError::create(m_realm, "Invalid algorithm parameters"_string);

        // 6. Let curvePrivateKey be the result of performing the parse an ASN.1 structure algorithm,
        //    with data as the privateKey field of privateKeyInfo, structure as the ASN.1 CurvePrivateKey
        //    structure specified in Section 7 of [RFC8410], and exactData set to true.
        // 7. If an error occurred while parsing, then throw a DataError.
        auto curve_private_key = TRY(parse_an_ASN1_structure<StringView>(m_realm, private_key_info.raw_key, true));
        auto curve_private_key_bytes = TRY_OR_THROW_OOM(m_realm->vm(), ByteBuffer::copy(curve_private_key.bytes()));

        // 8. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and that represents the X448 private key identified by curvePrivateKey.
        auto key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { curve_private_key_bytes });

        // 9. Set the [[type]] internal slot of key to "private"
        key->set_type(Bindings::KeyType::Private);

        // 10. Let algorithm be a new KeyAlgorithm.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 11. Set the name attribute of algorithm to "X448".
        algorithm->set_name("X448"_string);

        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);

        return key;
    }

    // 3. If format is "jwk":
    if (format == Bindings::KeyFormat::Jwk) {
        // 1. If keyData is a JsonWebKey dictionary:
        //    Let jwk equal keyData.
        //    Otherwise:
        //    Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "Data is not a JsonWebKey dictionary"_string);
        auto jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the d field is present and if usages contains an entry which is not "deriveKey" or "deriveBits" then throw a SyntaxError.
        if (jwk.d.has_value()) {
            for (auto const& usage : usages) {
                if (usage != Bindings::KeyUsage::Derivekey && usage != Bindings::KeyUsage::Derivebits) {
                    return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
                }
            }
        }

        // 3. If the d field is not present and if usages is not empty then throw a SyntaxError.
        if (!jwk.d.has_value() && !usages.is_empty())
            return WebIDL::SyntaxError::create(m_realm, "Usages must be empty"_string);

        // 4. If the kty field of jwk is not "OKP", then throw a DataError.
        if (jwk.kty != "OKP"sv)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 5. If the crv field of jwk is not "X448", then throw a DataError.
        if (jwk.crv != "X448"sv)
            return WebIDL::DataError::create(m_realm, "Invalid curve"_string);

        // 6. If usages is non-empty and the use field of jwk is present and is not equal to "enc" then throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && jwk.use.value() != "enc"sv)
            return WebIDL::DataError::create(m_realm, "Invalid use"_string);

        // 7. If the key_ops field of jwk is present, and is invalid according to the requirements of JSON Web Key [JWK],
        //    or it does not contain all of the specified usages values, then throw a DataError.
        TRY(validate_jwk_key_ops(m_realm, jwk, usages));

        // 8. If the ext field of jwk is present and has the value false and extractable is true, then throw a DataError.
        if (jwk.ext.has_value() && !jwk.ext.value() && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid extractable"_string);

        GC::Ptr<CryptoKey> key = nullptr;

        // 9. If the d field is present:
        if (jwk.d.has_value()) {
            // 1. If jwk does not meet the requirements of the JWK private key format described in Section 2 of [RFC8037], then throw a DataError.

            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

            // // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "X448"sv)
                return WebIDL::DataError::create(m_realm, "Invalid curve"_string);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create(m_realm, "Missing x field"_string);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (!jwk.d.has_value())
                return WebIDL::DataError::create(m_realm, "Missing d field"_string);

            // 2. Let key be a new CryptoKey object that represents the X25519 private key identified by interpreting jwk according to Section 2 of [RFC8037].
            auto private_key_base_64 = jwk.d.value();
            auto private_key_or_error = decode_base64url(private_key_base_64);
            if (private_key_or_error.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
            }
            auto private_key = private_key_or_error.release_value();
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { private_key });

            // 3. Set the [[type]] internal slot of Key to "private".
            key->set_type(Bindings::KeyType::Private);
        }
        // Otherwise:
        else {
            // 1. If jwk does not meet the requirements of the JWK public key format described in Section 2 of [RFC8037], then throw a DataError.
            // o  The parameter "kty" MUST be "OKP".
            if (jwk.kty != "OKP"sv)
                return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

            // https://www.iana.org/assignments/jose/jose.xhtml#web-key-elliptic-curve
            // o  The parameter "crv" MUST be present and contain the subtype of the key (from the "JSON Web Elliptic Curve" registry).
            if (jwk.crv != "X448"sv)
                return WebIDL::DataError::create(m_realm, "Invalid curve"_string);

            // o  The parameter "x" MUST be present and contain the public key encoded using the base64url [RFC4648] encoding.
            if (!jwk.x.has_value())
                return WebIDL::DataError::create(m_realm, "Missing x field"_string);

            // o  The parameter "d" MUST be present for private keys and contain the private key encoded using the base64url encoding.
            //    This parameter MUST NOT be present for public keys.
            if (jwk.d.has_value())
                return WebIDL::DataError::create(m_realm, "Present d field"_string);

            // 2. Let key be a new CryptoKey object that represents the Ed25519 public key identified by interpreting jwk according to Section 2 of [RFC8037].
            auto public_key_base_64 = jwk.x.value();
            auto public_key_or_error = decode_base64url(public_key_base_64);
            if (public_key_or_error.is_error()) {
                return WebIDL::DataError::create(m_realm, "Failed to decode base64"_string);
            }
            auto public_key = public_key_or_error.release_value();
            key = CryptoKey::create(m_realm, CryptoKey::InternalKeyData { public_key });

            // 3. Set the [[type]] internal slot of Key to "public".
            key->set_type(Bindings::KeyType::Public);
        }

        // 10. Let algorithm be a new instance of a KeyAlgorithm object.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 11. Set the name attribute of algorithm to "X448".
        algorithm->set_name("X448"_string);

        // 12. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);

        return GC::Ref { *key };
    }

    // 3. If format is "raw":
    if (format == Bindings::KeyFormat::Raw) {
        // 1. If usages is not empty then throw a SyntaxError.
        if (!usages.is_empty())
            return WebIDL::SyntaxError::create(m_realm, "Usages must be empty"_string);

        // 2. Let algorithm be a new KeyAlgorithm object.
        auto algorithm = KeyAlgorithm::create(m_realm);

        // 3. Set the name attribute of algorithm to "X448".
        algorithm->set_name("X448"_string);

        // 4. Let key be a new CryptoKey associated with the relevant global object of this [HTML], and representing the key data provided in keyData.
        auto key = CryptoKey::create(m_realm, key_data);

        // 5. Set the [[type]] internal slot of key to "public"
        key->set_type(Bindings::KeyType::Public);

        // 6. Set the [[algorithm]] internal slot of key to algorithm.
        key->set_algorithm(algorithm);

        return key;
    }

    (void)extractable;

    // 3. Otherwise:
    //    throw a NotSupportedError.
    return WebIDL::NotSupportedError::create(m_realm, "Invalid key format"_string);
}

static WebIDL::ExceptionOr<ByteBuffer> hmac_calculate_message_digest(JS::Realm& realm, GC::Ptr<KeyAlgorithm> hash, ReadonlyBytes key, ReadonlyBytes message)
{
    auto hash_name = hash->name();
    auto hash_kind = TRY([&] -> WebIDL::ExceptionOr<::Crypto::Hash::HashKind> {
        if (hash_name == "SHA-1")
            return ::Crypto::Hash::HashKind::SHA1;
        if (hash_name == "SHA-256")
            return ::Crypto::Hash::HashKind::SHA256;
        if (hash_name == "SHA-384")
            return ::Crypto::Hash::HashKind::SHA384;
        if (hash_name == "SHA-512")
            return ::Crypto::Hash::HashKind::SHA512;
        return WebIDL::NotSupportedError::create(realm, MUST(String::formatted("Invalid hash function '{}'", hash_name)));
    }());

    ::Crypto::Authentication::HMAC hmac(hash_kind, key);
    return hmac.process(message);
}

static WebIDL::ExceptionOr<WebIDL::UnsignedLong> hmac_hash_block_size(JS::Realm& realm, HashAlgorithmIdentifier hash)
{
    auto hash_name = TRY(hash.name(realm.vm()));
    if (hash_name == "SHA-1")
        return ::Crypto::Hash::SHA1::block_size();
    if (hash_name == "SHA-256")
        return ::Crypto::Hash::SHA256::block_size();
    if (hash_name == "SHA-384")
        return ::Crypto::Hash::SHA384::block_size();
    if (hash_name == "SHA-512")
        return ::Crypto::Hash::SHA512::block_size();
    return WebIDL::NotSupportedError::create(realm, MUST(String::formatted("Invalid hash function '{}'", hash_name)));
}

// https://w3c.github.io/webcrypto/#hmac-operations
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> HMAC::sign(AlgorithmParams const&, GC::Ref<CryptoKey> key, ByteBuffer const& message)
{
    // 1. Let mac be the result of performing the MAC Generation operation described in Section 4 of
    //    [FIPS-198-1] using the key represented by [[handle]] internal slot of key, the hash
    //    function identified by the hash attribute of the [[algorithm]] internal slot of key and
    //    message as the input data text.
    auto const& key_data = key->handle().get<ByteBuffer>();
    auto const& algorithm = as<HmacKeyAlgorithm>(*key->algorithm());
    auto mac = TRY(hmac_calculate_message_digest(m_realm, algorithm.hash(), key_data.bytes(), message.bytes()));

    // 2. Return the result of creating an ArrayBuffer containing mac.
    return JS::ArrayBuffer::create(m_realm, move(mac));
}

// https://w3c.github.io/webcrypto/#hmac-operations
WebIDL::ExceptionOr<JS::Value> HMAC::verify(AlgorithmParams const&, GC::Ref<CryptoKey> key, ByteBuffer const& signature, ByteBuffer const& message)
{
    // 1. Let mac be the result of performing the MAC Generation operation described in Section 4 of
    //    [FIPS-198-1] using the key represented by [[handle]] internal slot of key, the hash
    //    function identified by the hash attribute of the [[algorithm]] internal slot of key and
    //    message as the input data text.
    auto const& key_data = key->handle().get<ByteBuffer>();
    auto const& algorithm = as<HmacKeyAlgorithm>(*key->algorithm());
    auto mac = TRY(hmac_calculate_message_digest(m_realm, algorithm.hash(), key_data.bytes(), message.bytes()));

    // 2. Return true if mac is equal to signature and false otherwise.
    return mac == signature;
}

// https://w3c.github.io/webcrypto/#hmac-operations
WebIDL::ExceptionOr<Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>>> HMAC::generate_key(AlgorithmParams const& params, bool extractable, Vector<Bindings::KeyUsage> const& usages)
{
    // 1. If usages contains any entry which is not "sign" or "verify", then throw a SyntaxError.
    for (auto const& usage : usages) {
        if (usage != Bindings::KeyUsage::Sign && usage != Bindings::KeyUsage::Verify)
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
    }

    // 2. If the length member of normalizedAlgorithm is not present:
    auto const& normalized_algorithm = static_cast<HmacKeyGenParams const&>(params);
    WebIDL::UnsignedLong length;
    if (!normalized_algorithm.length.has_value()) {
        // Let length be the block size in bits of the hash function identified by the hash member
        // of normalizedAlgorithm.
        length = TRY(hmac_hash_block_size(m_realm, normalized_algorithm.hash)) * 8;
    }

    // Otherwise, if the length member of normalizedAlgorithm is non-zero:
    else if (normalized_algorithm.length.value() != 0) {
        // Let length be equal to the length member of normalizedAlgorithm.
        length = normalized_algorithm.length.value();
    }

    // Otherwise:
    else {
        // throw an OperationError.
        return WebIDL::OperationError::create(m_realm, "Invalid length"_string);
    }

    // 3. Generate a key of length length bits.
    auto key_data = MUST(generate_random_key(m_realm->vm(), length));

    // 4. If the key generation step fails, then throw an OperationError.
    // NOTE: Currently key generation must succeed

    // 5. Let key be a new CryptoKey object representing the generated key.
    auto key = CryptoKey::create(m_realm, move(key_data));

    // 6. Let algorithm be a new HmacKeyAlgorithm.
    auto algorithm = HmacKeyAlgorithm::create(m_realm);

    // 7. Set the name attribute of algorithm to "HMAC".
    algorithm->set_name("HMAC"_string);

    // 8. Let hash be a new KeyAlgorithm.
    auto hash = KeyAlgorithm::create(m_realm);

    // 9. Set the name attribute of hash to equal the name member of the hash member of normalizedAlgorithm.
    hash->set_name(TRY(normalized_algorithm.hash.name(m_realm->vm())));

    // 10. Set the hash attribute of algorithm to hash.
    algorithm->set_hash(hash);

    // 11. Set the length attribute of algorithm to length.
    algorithm->set_length(length);

    // 12. Set the [[type]] internal slot of key to "secret".
    key->set_type(Bindings::KeyType::Secret);

    // 13. Set the [[algorithm]] internal slot of key to algorithm.
    key->set_algorithm(algorithm);

    // 14. Set the [[extractable]] internal slot of key to be extractable.
    key->set_extractable(extractable);

    // 15. Set the [[usages]] internal slot of key to be usages.
    key->set_usages(usages);

    // 16. Return key.
    return Variant<GC::Ref<CryptoKey>, GC::Ref<CryptoKeyPair>> { key };
}

// https://w3c.github.io/webcrypto/#hmac-operations
WebIDL::ExceptionOr<GC::Ref<CryptoKey>> HMAC::import_key(Web::Crypto::AlgorithmParams const& params, Bindings::KeyFormat key_format, CryptoKey::InternalKeyData key_data, bool extractable, Vector<Bindings::KeyUsage> const& usages)
{
    auto& vm = m_realm->vm();
    auto const& normalized_algorithm = static_cast<HmacImportParams const&>(params);

    // 1. Let keyData be the key data to be imported.
    // 2. If usages contains an entry which is not "sign" or "verify", then throw a SyntaxError.
    for (auto const& usage : usages) {
        if (usage != Bindings::KeyUsage::Sign && usage != Bindings::KeyUsage::Verify)
            return WebIDL::SyntaxError::create(m_realm, MUST(String::formatted("Invalid key usage '{}'", idl_enum_to_string(usage))));
    }

    // 3. Let hash be a new KeyAlgorithm.
    auto hash = KeyAlgorithm::create(m_realm);

    // 4. If format is "raw":
    AK::ByteBuffer data;
    if (key_format == Bindings::KeyFormat::Raw) {
        // 4.1. Let data be the octet string contained in keyData.
        data = key_data.get<ByteBuffer>();

        // 4.2. Set hash to equal the hash member of normalizedAlgorithm.
        hash->set_name(TRY(normalized_algorithm.hash.name(vm)));
    }

    // If format is "jwk":
    else if (key_format == Bindings::KeyFormat::Jwk) {
        // 1. If keyData is a JsonWebKey dictionary:
        //    Let jwk equal keyData.
        //    Otherwise:
        //    Throw a DataError.
        if (!key_data.has<Bindings::JsonWebKey>())
            return WebIDL::DataError::create(m_realm, "Data is not a JsonWebKey dictionary"_string);
        auto jwk = key_data.get<Bindings::JsonWebKey>();

        // 2. If the kty field of jwk is not "oct", then throw a DataError.
        if (jwk.kty != "oct"sv)
            return WebIDL::DataError::create(m_realm, "Invalid key type"_string);

        // 3. If jwk does not meet the requirements of Section 6.4 of JSON Web Algorithms [JWA],
        //    then throw a DataError.
        // 4. Let data be the octet string obtained by decoding the k field of jwk.
        data = TRY(parse_jwk_symmetric_key(m_realm, jwk));

        // 5. Set the hash to equal the hash member of normalizedAlgorithm.
        hash->set_name(TRY(normalized_algorithm.hash.name(vm)));

        // 6. If the name attribute of hash is "SHA-1":
        auto hash_name = hash->name();
        if (hash_name == "SHA-1") {
            // If the alg field of jwk is present and is not "HS1", then throw a DataError.
            if (jwk.alg.has_value() && jwk.alg != "HS1"sv)
                return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // If the name attribute of hash is "SHA-256":
        else if (hash_name == "SHA-256") {
            // If the alg field of jwk is present and is not "HS256", then throw a DataError.
            if (jwk.alg.has_value() && jwk.alg != "HS256"sv)
                return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // If the name attribute of hash is "SHA-384":
        else if (hash_name == "SHA-384") {
            // If the alg field of jwk is present and is not "HS384", then throw a DataError.
            if (jwk.alg.has_value() && jwk.alg != "HS384"sv)
                return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // If the name attribute of hash is "SHA-512":
        else if (hash_name == "SHA-512") {
            // If the alg field of jwk is present and is not "HS512", then throw a DataError.
            if (jwk.alg.has_value() && jwk.alg != "HS512"sv)
                return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // FIXME: Otherwise, if the name attribute of hash is defined in another applicable specification:
        else {
            // FIXME: Perform any key import steps defined by other applicable specifications, passing format,
            //        jwk and hash and obtaining hash.
            dbgln("Hash algorithm '{}' not supported", hash_name);
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // 7. If usages is non-empty and the use field of jwk is present and is not "sign", then
        //    throw a DataError.
        if (!usages.is_empty() && jwk.use.has_value() && jwk.use != "sign"sv)
            return WebIDL::DataError::create(m_realm, "Invalid use in JsonWebKey"_string);

        // 8. If the key_ops field of jwk is present, and is invalid according to the requirements
        //    of JSON Web Key [JWK] or does not contain all of the specified usages values, then
        //    throw a DataError.
        TRY(validate_jwk_key_ops(m_realm, jwk, usages));

        // 9. If the ext field of jwk is present and has the value false and extractable is true,
        //    then throw a DataError.
        if (jwk.ext.has_value() && !*jwk.ext && extractable)
            return WebIDL::DataError::create(m_realm, "Invalid ext field"_string);
    }

    // Otherwise:
    else {
        // throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Invalid key format"_string);
    }

    // 5. Let length be equivalent to the length, in octets, of data, multiplied by 8.
    auto length = data.size() * 8;

    // 6. If length is zero then throw a DataError.
    if (length == 0)
        return WebIDL::DataError::create(m_realm, "No data provided"_string);

    // 7. If the length member of normalizedAlgorithm is present:
    if (normalized_algorithm.length.has_value()) {
        // If the length member of normalizedAlgorithm is greater than length:
        auto normalized_algorithm_length = normalized_algorithm.length.value();
        if (normalized_algorithm_length > length) {
            // throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid data size"_string);
        }

        // If the length member of normalizedAlgorithm, is less than or equal to length minus eight:
        if (normalized_algorithm_length <= length - 8) {
            // throw a DataError.
            return WebIDL::DataError::create(m_realm, "Invalid data size"_string);
        }

        // Otherwise:
        // Set length equal to the length member of normalizedAlgorithm.
        length = normalized_algorithm_length;
    }

    // 8. Let key be a new CryptoKey object representing an HMAC key with the first length bits of data.
    auto length_in_bytes = length / 8;
    if (data.size() > length_in_bytes)
        data = MUST(data.slice(0, length_in_bytes));
    auto key = CryptoKey::create(m_realm, move(data));

    // 9. Set the [[type]] internal slot of key to "secret".
    key->set_type(Bindings::KeyType::Secret);

    // 10. Let algorithm be a new HmacKeyAlgorithm.
    auto algorithm = HmacKeyAlgorithm::create(m_realm);

    // 11. Set the name attribute of algorithm to "HMAC".
    algorithm->set_name("HMAC"_string);

    // 12. Set the length attribute of algorithm to length.
    algorithm->set_length(length);

    // 13. Set the hash attribute of algorithm to hash.
    algorithm->set_hash(hash);

    // 14. Set the [[algorithm]] internal slot of key to algorithm.
    key->set_algorithm(algorithm);

    // 15. Return key.
    return key;
}

// https://w3c.github.io/webcrypto/#hmac-operations
WebIDL::ExceptionOr<GC::Ref<JS::Object>> HMAC::export_key(Bindings::KeyFormat format, GC::Ref<CryptoKey> key)
{
    // 1. If the underlying cryptographic key material represented by the [[handle]] internal slot
    //    of key cannot be accessed, then throw an OperationError.
    // NOTE: In our impl this is always accessible

    // 2. Let bits be the raw bits of the key represented by [[handle]] internal slot of key.
    // 3. Let data be an octet string containing bits.
    auto data = key->handle().get<ByteBuffer>();

    // 4. If format is "raw":
    GC::Ptr<JS::Object> result;
    if (format == Bindings::KeyFormat::Raw) {
        // Let result be the result of creating an ArrayBuffer containing data.
        result = JS::ArrayBuffer::create(m_realm, data);
    }

    // If format is "jwk":
    else if (format == Bindings::KeyFormat::Jwk) {
        // Let jwk be a new JsonWebKey dictionary.
        Bindings::JsonWebKey jwk {};

        // Set the kty attribute of jwk to the string "oct".
        jwk.kty = "oct"_string;

        // Set the k attribute of jwk to be a string containing data, encoded according to Section
        // 6.4 of JSON Web Algorithms [JWA].
        jwk.k = MUST(encode_base64url(data, AK::OmitPadding::Yes));

        // Let algorithm be the [[algorithm]] internal slot of key.
        auto const& algorithm = as<HmacKeyAlgorithm>(*key->algorithm());

        // Let hash be the hash attribute of algorithm.
        auto hash = algorithm.hash();

        // If the name attribute of hash is "SHA-1":
        auto hash_name = hash->name();
        if (hash_name == "SHA-1") {
            // Set the alg attribute of jwk to the string "HS1".
            jwk.alg = "HS1"_string;
        }
        // If the name attribute of hash is "SHA-256":
        else if (hash_name == "SHA-256") {
            // Set the alg attribute of jwk to the string "HS256".
            jwk.alg = "HS256"_string;
        }
        // If the name attribute of hash is "SHA-384":
        else if (hash_name == "SHA-384") {
            // Set the alg attribute of jwk to the string "HS384".
            jwk.alg = "HS384"_string;
        }
        // If the name attribute of hash is "SHA-512":
        else if (hash_name == "SHA-512") {
            // Set the alg attribute of jwk to the string "HS512".
            jwk.alg = "HS512"_string;
        }

        // FIXME: Otherwise, the name attribute of hash is defined in another applicable
        //        specification:
        else {
            // FIXME: Perform any key export steps defined by other applicable specifications,
            //        passing format and key and obtaining alg.
            // FIXME: Set the alg attribute of jwk to alg.
            dbgln("Hash algorithm '{}' not supported", hash_name);
            return WebIDL::DataError::create(m_realm, "Invalid algorithm"_string);
        }

        // Set the key_ops attribute of jwk to equal the usages attribute of key.
        jwk.key_ops = Vector<String> {};
        jwk.key_ops->ensure_capacity(key->internal_usages().size());
        for (auto const& usage : key->internal_usages()) {
            jwk.key_ops->append(Bindings::idl_enum_to_string(usage));
        }

        // Set the ext attribute of jwk to equal the [[extractable]] internal slot of key.
        jwk.ext = key->extractable();

        // Let result be the result of converting jwk to an ECMAScript Object, as defined by [WebIDL].
        result = TRY(jwk.to_object(m_realm));
    }

    // Otherwise:
    else {
        // throw a NotSupportedError.
        return WebIDL::NotSupportedError::create(m_realm, "Invalid key format"_string);
    }

    // 5. Return result.
    return GC::Ref { *result };
}

// https://w3c.github.io/webcrypto/#hmac-operations
WebIDL::ExceptionOr<JS::Value> HMAC::get_key_length(AlgorithmParams const& params)
{
    auto const& normalized_derived_key_algorithm = static_cast<HmacImportParams const&>(params);
    WebIDL::UnsignedLong length;

    // 1. If the length member of normalizedDerivedKeyAlgorithm is not present:
    if (!normalized_derived_key_algorithm.length.has_value()) {
        // Let length be the block size in bits of the hash function identified by the hash member of
        // normalizedDerivedKeyAlgorithm.
        length = TRY(hmac_hash_block_size(m_realm, normalized_derived_key_algorithm.hash)) * 8;
    }

    // Otherwise, if the length member of normalizedDerivedKeyAlgorithm is non-zero:
    else if (normalized_derived_key_algorithm.length.value() > 0) {
        // Let length be equal to the length member of normalizedDerivedKeyAlgorithm.
        length = normalized_derived_key_algorithm.length.value();
    }

    // Otherwise:
    else {
        // throw a TypeError.
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid key length"sv };
    }

    // 2. Return length.
    return JS::Value(length);
}
}
