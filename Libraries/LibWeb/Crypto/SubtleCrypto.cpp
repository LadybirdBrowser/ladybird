/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Curves/EdwardsCurve.h>
#include <LibCrypto/Curves/SECPxxxr1.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Crypto/SubtleCrypto.h>

namespace Web::Crypto {

enum class AlgorithmId : u8 {
    RSASSAPKCS1,
    RSAPSS,
    RSAOAEP,
    ECDSA,
    ECDH,
    AesCtr,
    AesCbc,
    AesGcm,
    AesKw,
    HMAC,
    SHA,
    CShake,
    HKDF,
    PBKDF2,
    X25519,
    X448,
    ED25519,
    ED448,
    MLDSA,
    MLKEM,
    Argon2,
    ChaCha20Poly1305,
    AesOcb,
    KMAC,
};

enum class ParameterId : u8 {
    AlgorithmParams,
    RsaHashedKeyGenParams,
    RsaHashedImportParams,
    RsaPssParams,
    RsaOaepParams,
    EcdsaParams,
    EcKeyGenParams,
    EcKeyImportParams,
    EcdhKeyDeriveParams,
    AesCtrParams,
    AesKeyGenParams,
    AesDerivedKeyParams,
    AesCbcParams,
    AesGcmParams,
    HmacKeyGenParams,
    HmacImportParams,
    CShakeParams,
    HKDFParams,
    PBKDF2Params,
    Ed448Params,
    ContextParams,
    Argon2Params,
    AeadParams,
    KmacParams,
    KmacKeyGenParams,
    KmacImportParams,
};

struct RawRsaOtherPrimesInfo {
    FlatPtr d;
    FlatPtr r;
    FlatPtr t;
};

struct RawJsonWebKey {
    FlatPtr alg;
    FlatPtr crv;
    FlatPtr d;
    FlatPtr dp;
    FlatPtr dq;
    FlatPtr e;
    FlatPtr k;
    FlatPtr kty;
    FlatPtr n;
    FlatPtr p;
    FlatPtr priv;
    FlatPtr pub;
    FlatPtr q;
    FlatPtr qi;
    FlatPtr use;
    FlatPtr x;
    FlatPtr y;
    u8 ext;
    bool has_key_ops;
    ReadonlySpan<FlatPtr> key_ops;
    bool has_oth;
    ReadonlySpan<RawRsaOtherPrimesInfo> oth;
};

struct OptionalLength {
    bool has_value;
    u32 value;

    Optional<u32> to_optional() const
    {
        if (has_value)
            return value;
        return {};
    }
};

using ReceiveBytes = void (*)(void*, ReadonlyBytes);
using ReceiveJsonWebKey = void (*)(void*, RawJsonWebKey const&);
using ReceiveUsages = void (*)(void*, ReadonlySpan<KeyUsage>);

extern "C" {
JSCompletion web_rust_crypto_normalize_an_algorithm(JSVM*, JS::Realm*, JSObject*, FlatPtr, ReadonlyBytes, NormalizedAlgorithmAndParameter**);
JSCompletion web_crypto_algorithm_from_value(JS::Realm*, AlgorithmId, ParameterId, JSValue, FlatPtr, NormalizedAlgorithmAndParameter**);
void web_crypto_algorithm_destroy(NormalizedAlgorithmAndParameter*);
void web_crypto_algorithm_public_key_usages(NormalizedAlgorithmAndParameter const*, void*, ReceiveUsages);
void web_crypto_normalized_key_usages(ReadonlySpan<KeyUsage>, void*, ReceiveUsages);
CryptoKey* web_crypto_public_key_for_private_key(CryptoKey const*);
JSCompletion web_crypto_algorithm_encrypt(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes data, void* state, ReceiveBytes receive);
JSCompletion web_crypto_algorithm_decrypt(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes data, void* state, ReceiveBytes receive);
JSCompletion web_crypto_algorithm_sign(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes data, void* state, ReceiveBytes receive);
JSCompletion web_crypto_algorithm_verify(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes signature, ReadonlyBytes data, bool* out);
JSCompletion web_crypto_algorithm_digest(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, ReadonlyBytes data, void* state, ReceiveBytes receive);
JSCompletion web_crypto_algorithm_derive_bits(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, OptionalLength length, void* state, ReceiveBytes receive);
JSCompletion web_crypto_algorithm_import_key(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, KeyFormat format, ReadonlyBytes key_data, RawJsonWebKey const* jwk, bool extractable, ReadonlySpan<KeyUsage> usages, CryptoKey** out);
JSCompletion web_crypto_algorithm_generate_key(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, bool extractable, ReadonlySpan<KeyUsage> usages, CryptoKey** out_key, CryptoKey** out_public_key, CryptoKey** out_private_key);
JSCompletion web_crypto_algorithm_export_key(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, KeyFormat format, CryptoKey* key, void* state, ReceiveBytes receive_bytes, ReceiveJsonWebKey receive_jwk);
JSCompletion web_crypto_algorithm_get_key_length(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, OptionalLength* out);
JSCompletion web_crypto_algorithm_get_shared_key_length(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, u32* out);
JSCompletion web_crypto_algorithm_wrap_key(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes data, void* state, ReceiveBytes receive);
JSCompletion web_crypto_algorithm_unwrap_key(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes data, void* state, ReceiveBytes receive);
JSCompletion web_crypto_algorithm_encapsulate(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, void* shared_key, void* ciphertext, ReceiveBytes receive);
JSCompletion web_crypto_algorithm_decapsulate(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes data, void* state, ReceiveBytes receive);
JSCompletion web_crypto_algorithm_validate_encrypt_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm);
JSCompletion web_crypto_algorithm_validate_decrypt_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm);
JSCompletion web_crypto_algorithm_validate_sign_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm);
JSCompletion web_crypto_algorithm_validate_verify_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm);
JSCompletion web_crypto_algorithm_validate_digest_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm);
JSCompletion web_crypto_algorithm_validate_generate_key_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm);
JSCompletion web_crypto_algorithm_validate_import_key_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm);
JSCompletion web_crypto_algorithm_validate_derive_bits_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, OptionalLength length);
}

static Optional<Utf16String> string_from_raw(FlatPtr raw)
{
    if (raw)
        return Utf16String::from_raw(raw);
    return {};
}

static JsonWebKey json_web_key_from_raw(RawJsonWebKey const& raw)
{
    JsonWebKey key;
    key.alg = string_from_raw(raw.alg);
    key.crv = string_from_raw(raw.crv);
    key.d = string_from_raw(raw.d);
    key.dp = string_from_raw(raw.dp);
    key.dq = string_from_raw(raw.dq);
    key.e = string_from_raw(raw.e);
    key.k = string_from_raw(raw.k);
    key.kty = string_from_raw(raw.kty);
    key.n = string_from_raw(raw.n);
    key.p = string_from_raw(raw.p);
    key.priv = string_from_raw(raw.priv);
    key.pub = string_from_raw(raw.pub);
    key.q = string_from_raw(raw.q);
    key.qi = string_from_raw(raw.qi);
    key.use = string_from_raw(raw.use);
    key.x = string_from_raw(raw.x);
    key.y = string_from_raw(raw.y);
    if (raw.ext)
        key.ext = raw.ext == 2;
    if (raw.has_key_ops) {
        key.key_ops = Vector<Utf16String> {};
        for (auto value : raw.key_ops)
            key.key_ops->append(Utf16String::from_raw(value));
    }
    if (raw.has_oth) {
        key.oth = Vector<RsaOtherPrimesInfo> {};
        for (auto const& prime : raw.oth)
            key.oth->append(RsaOtherPrimesInfo { string_from_raw(prime.d), string_from_raw(prime.r), string_from_raw(prime.t) });
    }
    return key;
}

static FlatPtr raw_string(Optional<Utf16String> const& string)
{
    return string.has_value() ? string->raw_identity() : 0;
}

static void with_raw_json_web_key(JsonWebKey const& key, void* state, ReceiveJsonWebKey receive)
{
    Vector<FlatPtr> key_ops;
    if (key.key_ops.has_value()) {
        for (auto const& value : *key.key_ops)
            key_ops.append(value.raw_identity());
    }
    Vector<RawRsaOtherPrimesInfo> oth;
    if (key.oth.has_value()) {
        for (auto const& prime : *key.oth)
            oth.append(RawRsaOtherPrimesInfo { raw_string(prime.d), raw_string(prime.r), raw_string(prime.t) });
    }
    RawJsonWebKey raw {
        raw_string(key.alg),
        raw_string(key.crv),
        raw_string(key.d),
        raw_string(key.dp),
        raw_string(key.dq),
        raw_string(key.e),
        raw_string(key.k),
        raw_string(key.kty),
        raw_string(key.n),
        raw_string(key.p),
        raw_string(key.priv),
        raw_string(key.pub),
        raw_string(key.q),
        raw_string(key.qi),
        raw_string(key.use),
        raw_string(key.x),
        raw_string(key.y),
        static_cast<u8>(key.ext.has_value() ? (*key.ext ? 2 : 1) : 0),
        key.key_ops.has_value(),
        key_ops.span(),
        key.oth.has_value(),
        oth.span(),
    };
    receive(state, raw);
}

WebIDL::ExceptionOr<NormalizedAlgorithmAndParameter> normalize_an_algorithm(JS::Realm& realm, AlgorithmIdentifier const& algorithm, String operation)
{
    JSObject* object = nullptr;
    FlatPtr name = 0;
    if (algorithm.has<Utf16String>())
        name = algorithm.get<Utf16String>().raw_identity();
    else
        object = JS::HostABI::object_to_abi(algorithm.get<GC::Ref<JS::Object>>().ptr());

    NormalizedAlgorithmAndParameter* normalized_algorithm = nullptr;
    TRY(JS::HostABI::completion_from_abi<void>(web_rust_crypto_normalize_an_algorithm(
        JS::HostABI::vm_to_abi(realm.vm()), &realm, object, name, operation.bytes(), &normalized_algorithm)));
    return move(*adopt_own(*normalized_algorithm));
}

JSCompletion web_crypto_algorithm_from_value(JS::Realm* realm_pointer, AlgorithmId method_id, ParameterId parameter_id, JSValue value, FlatPtr name, NormalizedAlgorithmAndParameter** out)
{
    auto& realm = *realm_pointer;
    auto methods = [&]() -> NonnullOwnPtr<AlgorithmMethods> {
        switch (method_id) {
        case AlgorithmId::RSASSAPKCS1:
            return RSASSAPKCS1::create();
        case AlgorithmId::RSAPSS:
            return RSAPSS::create();
        case AlgorithmId::RSAOAEP:
            return RSAOAEP::create();
        case AlgorithmId::ECDSA:
            return ECDSA::create();
        case AlgorithmId::ECDH:
            return ECDH::create();
        case AlgorithmId::AesCtr:
            return AesCtr::create();
        case AlgorithmId::AesCbc:
            return AesCbc::create();
        case AlgorithmId::AesGcm:
            return AesGcm::create();
        case AlgorithmId::AesKw:
            return AesKw::create();
        case AlgorithmId::HMAC:
            return HMAC::create();
        case AlgorithmId::SHA:
            return SHA::create();
        case AlgorithmId::CShake:
            return CShake::create();
        case AlgorithmId::HKDF:
            return HKDF::create();
        case AlgorithmId::PBKDF2:
            return PBKDF2::create();
        case AlgorithmId::X25519:
            return X25519::create();
        case AlgorithmId::X448:
            return X448::create();
        case AlgorithmId::ED25519:
            return ED25519::create();
        case AlgorithmId::ED448:
            return ED448::create();
        case AlgorithmId::MLDSA:
            return MLDSA::create();
        case AlgorithmId::MLKEM:
            return MLKEM::create();
        case AlgorithmId::Argon2:
            return Argon2::create();
        case AlgorithmId::ChaCha20Poly1305:
            return ChaCha20Poly1305::create();
        case AlgorithmId::AesOcb:
            return AesOcb::create();
        case AlgorithmId::KMAC:
            return KMAC::create();
        }
        VERIFY_NOT_REACHED();
    }();
    auto parameter_or_error = [&]() -> JS::ThrowCompletionOr<NonnullOwnPtr<AlgorithmParams>> {
        auto object = JS::HostABI::value_from_abi(value);
        switch (parameter_id) {
        case ParameterId::AlgorithmParams:
            return AlgorithmParams::from_value(realm, object);
        case ParameterId::RsaHashedKeyGenParams:
            return RsaHashedKeyGenParams::from_value(realm, object);
        case ParameterId::RsaHashedImportParams:
            return RsaHashedImportParams::from_value(realm, object);
        case ParameterId::RsaPssParams:
            return RsaPssParams::from_value(realm, object);
        case ParameterId::RsaOaepParams:
            return RsaOaepParams::from_value(realm, object);
        case ParameterId::EcdsaParams:
            return EcdsaParams::from_value(realm, object);
        case ParameterId::EcKeyGenParams:
            return EcKeyGenParams::from_value(realm, object);
        case ParameterId::EcKeyImportParams:
            return EcKeyImportParams::from_value(realm, object);
        case ParameterId::EcdhKeyDeriveParams:
            return EcdhKeyDeriveParams::from_value(realm, object);
        case ParameterId::AesCtrParams:
            return AesCtrParams::from_value(realm, object);
        case ParameterId::AesKeyGenParams:
            return AesKeyGenParams::from_value(realm, object);
        case ParameterId::AesDerivedKeyParams:
            return AesDerivedKeyParams::from_value(realm, object);
        case ParameterId::AesCbcParams:
            return AesCbcParams::from_value(realm, object);
        case ParameterId::AesGcmParams:
            return AesGcmParams::from_value(realm, object);
        case ParameterId::HmacKeyGenParams:
            return HmacKeyGenParams::from_value(realm, object);
        case ParameterId::HmacImportParams:
            return HmacImportParams::from_value(realm, object);
        case ParameterId::CShakeParams:
            return CShakeParams::from_value(realm, object);
        case ParameterId::HKDFParams:
            return HKDFParams::from_value(realm, object);
        case ParameterId::PBKDF2Params:
            return PBKDF2Params::from_value(realm, object);
        case ParameterId::Ed448Params:
            return Ed448Params::from_value(realm, object);
        case ParameterId::ContextParams:
            return ContextParams::from_value(realm, object);
        case ParameterId::Argon2Params:
            return Argon2Params::from_value(realm, object);
        case ParameterId::AeadParams:
            return AeadParams::from_value(realm, object);
        case ParameterId::KmacParams:
            return KmacParams::from_value(realm, object);
        case ParameterId::KmacKeyGenParams:
            return KmacKeyGenParams::from_value(realm, object);
        case ParameterId::KmacImportParams:
            return KmacImportParams::from_value(realm, object);
        }
        VERIFY_NOT_REACHED();
    }();
    if (parameter_or_error.is_error())
        return JS::HostABI::throw_completion_to_abi(parameter_or_error.release_error().value());
    auto parameter = parameter_or_error.release_value();
    VERIFY(parameter->name.is_empty());
    parameter->name = Utf16String::from_raw(name);
    *out = new NormalizedAlgorithmAndParameter { move(methods), move(parameter) };
    return JS::HostABI::normal_completion_to_abi(0);
}

void web_crypto_algorithm_destroy(NormalizedAlgorithmAndParameter* algorithm)
{
    delete algorithm;
}

void web_crypto_algorithm_public_key_usages(NormalizedAlgorithmAndParameter const* algorithm, void* state, ReceiveUsages receive)
{
    if (auto usages = algorithm->methods->public_key_usages(); usages.has_value())
        receive(state, *usages);
}

void web_crypto_normalized_key_usages(ReadonlySpan<KeyUsage> usages, void* state, ReceiveUsages receive)
{
    auto normalized = normalized_key_usages(usages);
    receive(state, normalized.span());
}

static ErrorOr<CryptoKey::InternalKeyData> public_key_for_private_key(CryptoKey const& key)
{
    return key.handle().visit(
        [](::Crypto::PK::RSAPrivateKey const& private_key) -> ErrorOr<CryptoKey::InternalKeyData> {
            return ::Crypto::PK::RSAPublicKey { private_key.modulus(), private_key.public_exponent() };
        },
        [&](::Crypto::PK::ECPrivateKey const& private_key) -> ErrorOr<CryptoKey::InternalKeyData> {
            if (private_key.public_key().has_value())
                return *private_key.public_key();

            auto algorithm = key.ec_algorithm();
            auto const& named_curve = algorithm.named_curve();
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
        [](::Crypto::PK::MLDSAPrivateKey const& private_key) -> ErrorOr<CryptoKey::InternalKeyData> {
            return ::Crypto::PK::MLDSAPublicKey { private_key.public_key() };
        },
        [](::Crypto::PK::MLKEMPrivateKey const& private_key) -> ErrorOr<CryptoKey::InternalKeyData> {
            return ::Crypto::PK::MLKEMPublicKey { private_key.public_key() };
        },
        [&](OKPPrivateKey const& private_key) -> ErrorOr<CryptoKey::InternalKeyData> {
            auto const& name = key.algorithm_name();
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
        [](auto const&) -> ErrorOr<CryptoKey::InternalKeyData> {
            return Error::from_string_literal("Key is not a private key");
        });
}

CryptoKey* web_crypto_public_key_for_private_key(CryptoKey const* key)
{
    auto data = public_key_for_private_key(*key);
    if (data.is_error())
        return nullptr;
    return CryptoKey::create(data.release_value()).ptr();
}

JSCompletion web_crypto_algorithm_encrypt(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes data, void* state, ReceiveBytes receive)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->encrypt(realm, *algorithm->parameter, *key, MUST(ByteBuffer::copy(data)));
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    receive(state, result.value().span());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_decrypt(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes data, void* state, ReceiveBytes receive)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->decrypt(realm, *algorithm->parameter, *key, MUST(ByteBuffer::copy(data)));
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    receive(state, result.value().span());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_sign(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes data, void* state, ReceiveBytes receive)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->sign(realm, *algorithm->parameter, *key, MUST(ByteBuffer::copy(data)));
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    receive(state, result.value().span());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_verify(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes signature, ReadonlyBytes data, bool* out)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->verify(realm, *algorithm->parameter, *key, MUST(ByteBuffer::copy(signature)), MUST(ByteBuffer::copy(data)));
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    *out = result.value();
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_digest(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, ReadonlyBytes data, void* state, ReceiveBytes receive)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->digest(realm, *algorithm->parameter, MUST(ByteBuffer::copy(data)));
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    receive(state, result.value().span());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_derive_bits(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, OptionalLength length, void* state, ReceiveBytes receive)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->derive_bits(realm, *algorithm->parameter, *key, length.to_optional());
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    receive(state, result.value().span());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_import_key(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, KeyFormat format, ReadonlyBytes key_data, RawJsonWebKey const* jwk, bool extractable, ReadonlySpan<KeyUsage> usages, CryptoKey** out)
{
    auto& realm = *realm_pointer;
    Vector<KeyUsage> usage_vector;
    usage_vector.append(usages.data(), usages.size());
    CryptoKey::ImportKeyData data = jwk ? CryptoKey::ImportKeyData { json_web_key_from_raw(*jwk) } : CryptoKey::ImportKeyData { MUST(ByteBuffer::copy(key_data)) };
    auto result = algorithm->methods->import_key(realm, *algorithm->parameter, format, move(data), extractable, usage_vector);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    *out = result.value().ptr();
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_generate_key(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, bool extractable, ReadonlySpan<KeyUsage> usages, CryptoKey** out_key, CryptoKey** out_public_key, CryptoKey** out_private_key)
{
    auto& realm = *realm_pointer;
    Vector<KeyUsage> usage_vector;
    usage_vector.append(usages.data(), usages.size());
    auto result = algorithm->methods->generate_key(realm, *algorithm->parameter, extractable, usage_vector);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    result.value().visit(
        [&](GC::Ref<CryptoKey> const& key) { *out_key = key.ptr(); },
        [&](CryptoKeyPair const& pair) {
            *out_public_key = pair.public_key.ptr();
            *out_private_key = pair.private_key.ptr();
        });
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_export_key(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, KeyFormat format, CryptoKey* key, void* state, ReceiveBytes receive_bytes, ReceiveJsonWebKey receive_jwk)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->export_key(realm, format, *key);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    result.value().visit(
        [&](ByteBuffer const& bytes) { receive_bytes(state, bytes.span()); },
        [&](JsonWebKey const& key) { with_raw_json_web_key(key, state, receive_jwk); });
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_get_key_length(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, OptionalLength* out)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->get_key_length(realm, *algorithm->parameter);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    *out = { result.value().has_value(), result.value().value_or(0) };
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_get_shared_key_length(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, u32* out)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->get_shared_key_length(*algorithm->parameter);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    *out = result.value();
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_wrap_key(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes data, void* state, ReceiveBytes receive)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->wrap_key(realm, *algorithm->parameter, *key, MUST(ByteBuffer::copy(data)));
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    receive(state, result.value().span());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_unwrap_key(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes data, void* state, ReceiveBytes receive)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->unwrap_key(realm, *algorithm->parameter, *key, MUST(ByteBuffer::copy(data)));
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    receive(state, result.value().span());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_encapsulate(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, void* shared_key, void* ciphertext, ReceiveBytes receive)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->encapsulate(realm, *algorithm->parameter, *key);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    if (result.value().shared_key.has_value())
        receive(shared_key, result.value().shared_key->span());
    if (result.value().ciphertext.has_value())
        receive(ciphertext, result.value().ciphertext->span());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_decapsulate(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, CryptoKey* key, ReadonlyBytes data, void* state, ReceiveBytes receive)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->decapsulate(realm, *algorithm->parameter, *key, MUST(ByteBuffer::copy(data)));
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    receive(state, result.value().span());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_validate_encrypt_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->validate_encrypt_parameters(*algorithm->parameter);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_validate_decrypt_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->validate_decrypt_parameters(*algorithm->parameter);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_validate_sign_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->validate_sign_parameters(*algorithm->parameter);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_validate_verify_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->validate_verify_parameters(*algorithm->parameter);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_validate_digest_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->validate_digest_parameters(*algorithm->parameter);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_validate_generate_key_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->validate_generate_key_parameters(*algorithm->parameter);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_validate_import_key_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->validate_import_key_parameters(*algorithm->parameter);
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    return JS::HostABI::normal_completion_to_abi(0);
}

JSCompletion web_crypto_algorithm_validate_derive_bits_parameters(JS::Realm* realm_pointer, NormalizedAlgorithmAndParameter const* algorithm, OptionalLength length)
{
    auto& realm = *realm_pointer;
    auto result = algorithm->methods->validate_derive_bits_parameters(*algorithm->parameter, length.to_optional());
    if (result.is_exception())
        return Bindings::completion_to_abi(realm, result.release_error());
    return JS::HostABI::normal_completion_to_abi(0);
}

}
