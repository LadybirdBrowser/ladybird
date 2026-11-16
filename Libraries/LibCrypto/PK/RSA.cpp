/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Memory.h>
#include <AK/NumericLimits.h>
#include <AK/ScopeGuard.h>
#include <LibCrypto/OpenSSL.h>
#include <LibCrypto/PK/RSA.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace Crypto::PK {

static ErrorOr<ByteBuffer> integer_parameter(EVP_PKEY* key, char const* name)
{
    BIGNUM* value = nullptr;
    OPENSSL_TRY(EVP_PKEY_get_bn_param(key, name, &value));
    ScopeGuard const free_value = [&] { BN_clear_free(value); };

    auto bytes = TRY(ByteBuffer::create_uninitialized(BN_num_bytes(value)));
    BN_bn2bin(value, bytes.data());
    return bytes;
}

// NB: The builder reads each BIGNUM when it builds the parameters, so the BIGNUMs must outlive the builder.
class ParameterBuilder {
    AK_MAKE_NONCOPYABLE(ParameterBuilder);
    AK_MAKE_NONMOVABLE(ParameterBuilder);

public:
    ParameterBuilder() = default;

    ~ParameterBuilder()
    {
        OSSL_PARAM_BLD_free(m_builder);
        for (auto* integer : m_integers)
            BN_clear_free(integer);
    }

    ErrorOr<void> push_integer(char const* name, ReadonlyBytes bytes, bool secret)
    {
        if (!m_builder)
            m_builder = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());

        auto* integer = OPENSSL_TRY_PTR(secret ? BN_secure_new() : BN_new());
        TRY(m_integers.try_append(integer));
        OPENSSL_TRY_PTR(BN_bin2bn(bytes.data(), bytes.size(), integer));
        OPENSSL_TRY(OSSL_PARAM_BLD_push_BN(m_builder, name, integer));
        return {};
    }

    ErrorOr<void> push_size(char const* name, size_t value)
    {
        if (!m_builder)
            m_builder = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
        OPENSSL_TRY(OSSL_PARAM_BLD_push_size_t(m_builder, name, value));
        return {};
    }

    OSSL_PARAM_BLD* ptr() { return m_builder; }

private:
    OSSL_PARAM_BLD* m_builder { nullptr };
    Vector<BIGNUM*, 8> m_integers;
};

static ErrorOr<OpenSSL_SharedPKEY> key_from_parameters(int selection, ParameterBuilder& builder)
{
    auto* params = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_to_param(builder.ptr()));
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)));
    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx.ptr()));

    EVP_PKEY* key = nullptr;
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx.ptr(), &key, selection, params));
    return OpenSSL_SharedPKEY { TRY(OpenSSL_PKEY::wrap(key)) };
}

// https://www.rfc-editor.org/rfc/rfc8017#section-3.1
// [...] the RSA public exponent e is an integer between 3 and n - 1 [...]
// NB: An odd e above 1 is at least 3.
static ErrorOr<bool> public_exponent_is_valid(EVP_PKEY* key)
{
    BIGNUM* n = nullptr;
    BIGNUM* e = nullptr;
    ScopeGuard const free_values = [&] {
        BN_free(n);
        BN_free(e);
    };
    OPENSSL_TRY(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &n));
    OPENSSL_TRY(EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &e));
    return BN_is_odd(e) && !BN_is_one(e) && BN_cmp(e, n) < 0;
}

static ErrorOr<ByteBuffer> export_type_specific_der(EVP_PKEY* key, bool is_private)
{
    u8* der = nullptr;
    auto size = is_private ? i2d_PrivateKey(key, &der) : i2d_PublicKey(key, &der);
    if (size <= 0)
        return Error::from_string_literal("Failed to encode the RSA key");
    ScopeGuard const free_der = [&] { OPENSSL_clear_free(der, size); };
    return ByteBuffer::copy(der, size);
}

RSAPublicKey::RSAPublicKey(OpenSSL_SharedPKEY key)
    : m_key(move(key))
    , m_length(EVP_PKEY_get_size(m_key.ptr()))
{
}

ErrorOr<RSAPublicKey> RSAPublicKey::from_components(ReadonlyBytes modulus, ReadonlyBytes public_exponent)
{
    ParameterBuilder builder;
    TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_N, modulus, false));
    TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_E, public_exponent, false));
    return RSAPublicKey { TRY(key_from_parameters(EVP_PKEY_PUBLIC_KEY, builder)) };
}

size_t RSAPublicKey::modulus_bits() const
{
    return EVP_PKEY_get_bits(m_key.ptr());
}

ErrorOr<ByteBuffer> RSAPublicKey::modulus() const
{
    return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_RSA_N);
}

ErrorOr<ByteBuffer> RSAPublicKey::public_exponent() const
{
    return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_RSA_E);
}

ErrorOr<bool> RSAPublicKey::is_valid() const
{
    if (!TRY(public_exponent_is_valid(m_key.ptr())))
        return false;

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, m_key.ptr(), nullptr)));
    return EVP_PKEY_public_check(ctx.ptr()) == 1;
}

// https://www.rfc-editor.org/rfc/rfc8017#appendix-A.1.1
ErrorOr<ByteBuffer> RSAPublicKey::export_as_der() const
{
    return export_type_specific_der(m_key.ptr(), false);
}

RSAPrivateKey::RSAPrivateKey(OpenSSL_SharedPKEY key, bool has_crt_components)
    : m_key(move(key))
    , m_length(EVP_PKEY_get_size(m_key.ptr()))
    , m_has_crt_components(has_crt_components)
{
}

ErrorOr<RSAPrivateKey> RSAPrivateKey::from_components(ReadonlyBytes modulus, ReadonlyBytes public_exponent, ReadonlyBytes private_exponent, Optional<RSACRTComponents> crt_components)
{
    ParameterBuilder builder;
    TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_N, modulus, false));
    TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_E, public_exponent, false));
    TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_D, private_exponent, true));
    if (crt_components.has_value()) {
        TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_FACTOR1, crt_components->prime1, true));
        TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_FACTOR2, crt_components->prime2, true));
        TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_EXPONENT1, crt_components->exponent1, true));
        TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_EXPONENT2, crt_components->exponent2, true));
        TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_COEFFICIENT1, crt_components->coefficient, true));
    }
    return RSAPrivateKey { TRY(key_from_parameters(EVP_PKEY_KEYPAIR, builder)), crt_components.has_value() };
}

RSAPrivateKey RSAPrivateKey::from_openssl_key(OpenSSL_SharedPKEY key)
{
    BIGNUM* prime = nullptr;
    bool has_crt_components = EVP_PKEY_get_bn_param(key.ptr(), OSSL_PKEY_PARAM_RSA_FACTOR1, &prime) == 1;
    BN_clear_free(prime);
    return RSAPrivateKey { move(key), has_crt_components };
}

size_t RSAPrivateKey::modulus_bits() const
{
    return EVP_PKEY_get_bits(m_key.ptr());
}

ErrorOr<ByteBuffer> RSAPrivateKey::component(Component component) const
{
    switch (component) {
    case Component::Modulus:
        return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_RSA_N);
    case Component::PublicExponent:
        return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_RSA_E);
    case Component::PrivateExponent:
        return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_RSA_D);
    default:
        break;
    }

    if (!m_has_crt_components)
        return ByteBuffer {};

    switch (component) {
    case Component::Prime1:
        return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_RSA_FACTOR1);
    case Component::Prime2:
        return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_RSA_FACTOR2);
    case Component::Exponent1:
        return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_RSA_EXPONENT1);
    case Component::Exponent2:
        return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_RSA_EXPONENT2);
    case Component::Coefficient:
        return integer_parameter(m_key.ptr(), OSSL_PKEY_PARAM_RSA_COEFFICIENT1);
    default:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<RSAPublicKey> RSAPrivateKey::public_key() const
{
    auto modulus = TRY(component(Component::Modulus));
    auto public_exponent = TRY(component(Component::PublicExponent));
    return RSAPublicKey::from_components(modulus, public_exponent);
}

ErrorOr<bool> RSAPrivateKey::is_valid() const
{
    if (!TRY(public_exponent_is_valid(m_key.ptr())))
        return false;

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, m_key.ptr(), nullptr)));
    if (EVP_PKEY_public_check(ctx.ptr()) != 1 || EVP_PKEY_private_check(ctx.ptr()) != 1)
        return false;

    // NB: The pairwise check needs the prime factors, which a key of only n, e and d does not have.
    if (!m_has_crt_components)
        return true;
    return EVP_PKEY_pairwise_check(ctx.ptr()) == 1;
}

// https://www.rfc-editor.org/rfc/rfc8017#appendix-A.1.2
ErrorOr<ByteBuffer> RSAPrivateKey::export_as_der() const
{
    if (!m_has_crt_components)
        return Error::from_string_literal("Cannot export private key without prime factors");
    return export_type_specific_der(m_key.ptr(), true);
}

ErrorOr<void> RSA::validate_key_generation_parameters(size_t bits)
{
    // OpenSSL refuses to generate keys with shorter moduli.
    static constexpr size_t minimum_modulus_length = 512;

    if (bits < minimum_modulus_length)
        return Error::from_string_literal("RSA modulus length must be at least 512 bits");

    if (bits > OPENSSL_RSA_MAX_MODULUS_BITS)
        return Error::from_string_literal("RSA modulus length must be at most 16384 bits");

    return {};
}

ErrorOr<RSA::KeyPairType> RSA::generate_key_pair(size_t bits, ReadonlyBytes public_exponent)
{
    TRY(validate_key_generation_parameters(bits));

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)));
    OPENSSL_TRY(EVP_PKEY_keygen_init(ctx.ptr()));

    ParameterBuilder builder;
    TRY(builder.push_size(OSSL_PKEY_PARAM_RSA_BITS, bits));
    TRY(builder.push_integer(OSSL_PKEY_PARAM_RSA_E, public_exponent, false));

    auto* params = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_to_param(builder.ptr()));
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };
    OPENSSL_TRY(EVP_PKEY_CTX_set_params(ctx.ptr(), params));

    EVP_PKEY* key = nullptr;
    OPENSSL_TRY(EVP_PKEY_generate(ctx.ptr(), &key));

    RSAPrivateKey private_key { OpenSSL_SharedPKEY { TRY(OpenSSL_PKEY::wrap(key)) }, true };
    auto public_key = TRY(private_key.public_key());
    return KeyPairType { move(public_key), move(private_key) };
}

ErrorOr<RSA::KeyPairType> RSA::generate_key_pair(size_t bits)
{
    static constexpr u8 f4[] { 0x01, 0x00, 0x01 };
    return generate_key_pair(bits, f4);
}

static ErrorOr<Array<OSSL_PARAM, 5>> oaep_parameters(Hash::HashKind hash_kind, ReadonlyBytes const& label)
{
    auto* hash_name = const_cast<char*>(TRY(Hash::openssl_digest_name(hash_kind)));
    Array<OSSL_PARAM, 5> params {
        OSSL_PARAM_construct_utf8_string(OSSL_ASYM_CIPHER_PARAM_PAD_MODE, const_cast<char*>(OSSL_PKEY_RSA_PAD_MODE_OAEP), 0),
        OSSL_PARAM_construct_utf8_string(OSSL_ASYM_CIPHER_PARAM_OAEP_DIGEST, hash_name, 0),
        OSSL_PARAM_construct_utf8_string(OSSL_ASYM_CIPHER_PARAM_MGF1_DIGEST, hash_name, 0),
        OSSL_PARAM_construct_end(),
        OSSL_PARAM_construct_end(),
    };
    // NB: OpenSSL copies the label.
    if (!label.is_empty())
        params[3] = OSSL_PARAM_construct_octet_string(OSSL_ASYM_CIPHER_PARAM_OAEP_LABEL, const_cast<u8*>(label.data()), label.size());
    return params;
}

ErrorOr<ByteBuffer> rsa_oaep_encrypt(RSAPublicKey const& public_key, Hash::HashKind hash_kind, ReadonlyBytes label, ReadonlyBytes plaintext)
{
    auto* key = public_key.openssl_key();
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key, nullptr)));

    auto params = TRY(oaep_parameters(hash_kind, label));
    OPENSSL_TRY(EVP_PKEY_encrypt_init_ex(ctx.ptr(), params.data()));

    size_t out_size = 0;
    OPENSSL_TRY(EVP_PKEY_encrypt(ctx.ptr(), nullptr, &out_size, plaintext.data(), plaintext.size()));

    auto out = TRY(ByteBuffer::create_uninitialized(out_size));
    OPENSSL_TRY(EVP_PKEY_encrypt(ctx.ptr(), out.data(), &out_size, plaintext.data(), plaintext.size()));
    return out.slice(0, out_size);
}

ErrorOr<ByteBuffer> rsa_oaep_decrypt(RSAPrivateKey const& private_key, Hash::HashKind hash_kind, ReadonlyBytes label, ReadonlyBytes ciphertext)
{
    if (ciphertext.size() != private_key.length())
        return Error::from_string_literal("RSA ciphertext has an invalid length");

    auto* key = private_key.openssl_key();
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key, nullptr)));

    auto params = TRY(oaep_parameters(hash_kind, label));
    OPENSSL_TRY(EVP_PKEY_decrypt_init_ex(ctx.ptr(), params.data()));

    size_t out_size = 0;
    OPENSSL_TRY(EVP_PKEY_decrypt(ctx.ptr(), nullptr, &out_size, ciphertext.data(), ciphertext.size()));

    auto out = TRY(ByteBuffer::create_uninitialized(out_size));
    ScopeGuard const clear_out = [&] { secure_zero(out.data(), out.size()); };
    OPENSSL_TRY(EVP_PKEY_decrypt(ctx.ptr(), out.data(), &out_size, ciphertext.data(), ciphertext.size()));
    return ByteBuffer::copy(out.bytes().trim(out_size));
}

static ErrorOr<ByteBuffer> sign(RSAPrivateKey const& private_key, Hash::HashKind hash_kind, OSSL_PARAM const* params, ReadonlyBytes message)
{
    auto* key = private_key.openssl_key();
    auto ctx = TRY(OpenSSL_MD_CTX::create());

    OPENSSL_TRY(EVP_DigestSignInit_ex(ctx.ptr(), nullptr, TRY(Hash::openssl_digest_name(hash_kind)), nullptr, nullptr, key, params));

    size_t signature_size = 0;
    OPENSSL_TRY(EVP_DigestSign(ctx.ptr(), nullptr, &signature_size, message.data(), message.size()));

    auto signature = TRY(ByteBuffer::create_uninitialized(signature_size));
    OPENSSL_TRY(EVP_DigestSign(ctx.ptr(), signature.data(), &signature_size, message.data(), message.size()));
    return signature.slice(0, signature_size);
}

static ErrorOr<bool> verify(RSAPublicKey const& public_key, Hash::HashKind hash_kind, OSSL_PARAM const* params, ReadonlyBytes message, ReadonlyBytes signature)
{
    if (signature.size() != public_key.length())
        return false;

    // NB: Keys are checked in full when they are imported, so this only keeps the rule on e, which is cheap.
    if (!TRY(public_exponent_is_valid(public_key.openssl_key())))
        return false;

    auto* key = public_key.openssl_key();
    auto ctx = TRY(OpenSSL_MD_CTX::create());

    OPENSSL_TRY(EVP_DigestVerifyInit_ex(ctx.ptr(), nullptr, TRY(Hash::openssl_digest_name(hash_kind)), nullptr, nullptr, key, params));

    auto result = EVP_DigestVerify(ctx.ptr(), signature.data(), signature.size(), message.data(), message.size());
    if (result == 1)
        return true;
    if (result == 0)
        return false;
    OPENSSL_TRY(result);
    VERIFY_NOT_REACHED();
}

// NB: OpenSSL takes the salt length as an int and gives negative values special meanings, such as salt length
//     auto-detection, so a length that does not fit in an int must never reach it. No RSA key is large enough
//     for such a salt.
static bool salt_length_fits_in_int(u32 salt_length)
{
    return salt_length <= static_cast<u32>(NumericLimits<int>::max());
}

static ErrorOr<Array<OSSL_PARAM, 4>> pss_parameters(Hash::HashKind hash_kind, int& salt_length)
{
    return Array<OSSL_PARAM, 4> {
        OSSL_PARAM_construct_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, const_cast<char*>(OSSL_PKEY_RSA_PAD_MODE_PSS), 0),
        OSSL_PARAM_construct_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_DIGEST, const_cast<char*>(TRY(Hash::openssl_digest_name(hash_kind))), 0),
        OSSL_PARAM_construct_int(OSSL_SIGNATURE_PARAM_PSS_SALTLEN, &salt_length),
        OSSL_PARAM_construct_end(),
    };
}

ErrorOr<ByteBuffer> rsa_pss_sign(RSAPrivateKey const& private_key, Hash::HashKind hash_kind, u32 salt_length, ReadonlyBytes message)
{
    if (!salt_length_fits_in_int(salt_length))
        return Error::from_string_literal("Salt length is too large");

    auto salt_length_int = static_cast<int>(salt_length);
    auto params = TRY(pss_parameters(hash_kind, salt_length_int));
    return sign(private_key, hash_kind, params.data(), message);
}

ErrorOr<bool> rsa_pss_verify(RSAPublicKey const& public_key, Hash::HashKind hash_kind, u32 salt_length, ReadonlyBytes message, ReadonlyBytes signature)
{
    if (!salt_length_fits_in_int(salt_length))
        return false;

    auto salt_length_int = static_cast<int>(salt_length);
    auto params = TRY(pss_parameters(hash_kind, salt_length_int));
    return verify(public_key, hash_kind, params.data(), message, signature);
}

static OSSL_PARAM const pkcs1_parameters[] {
    OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, const_cast<char*>(OSSL_PKEY_RSA_PAD_MODE_PKCSV15), sizeof(OSSL_PKEY_RSA_PAD_MODE_PKCSV15) - 1),
    OSSL_PARAM_END,
};

ErrorOr<ByteBuffer> rsa_pkcs1_sign(RSAPrivateKey const& private_key, Hash::HashKind hash_kind, ReadonlyBytes message)
{
    return sign(private_key, hash_kind, pkcs1_parameters, message);
}

ErrorOr<bool> rsa_pkcs1_verify(RSAPublicKey const& public_key, Hash::HashKind hash_kind, ReadonlyBytes message, ReadonlyBytes signature)
{
    return verify(public_key, hash_kind, pkcs1_parameters, message, signature);
}

}
