/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/PK/MLDSA.h>

#include <LibCrypto/Curves/SECPxxxr1.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace Crypto::PK {

namespace {

char const* mldsa_size_to_openssl_name(MLDSASize size)
{
    switch (size) {
    case MLDSA44:
        return "ML-DSA-44";
    case MLDSA65:
        return "ML-DSA-65";
    case MLDSA87:
        return "ML-DSA-87";
    default:
        VERIFY_NOT_REACHED();
    }
}

}

static ErrorOr<ByteBuffer> read_mldsa_seed(ASN1::Decoder& decoder, Vector<StringView>& current_scope)
{
    // seed ::= OCTET STRING (SIZE (32))
    READ_OBJECT(OctetString, StringView, seed_bits);

    auto const seed = seed_bits.bytes();
    if (seed.size() != 32) {
        ERROR_WITH_SCOPE("Invalid seed length");
    }
    POP_SCOPE();

    return ByteBuffer::copy(seed);
}

static ErrorOr<ByteBuffer> read_mldsa_private_key(MLDSASize size, ASN1::Decoder& decoder, Vector<StringView>& current_scope)
{
    // expandedKey ::= OCTET STRING (SIZE (2560 | 4032 | 4896))

    ENTER_TYPED_SCOPE(OctetString, "expandedKey");
    READ_OBJECT(OctetString, StringView, expanded_key_bits);

    auto const expanded_key = expanded_key_bits.bytes();
    switch (size) {
    case MLDSA44:
        if (expanded_key.size() != 2560) {
            ERROR_WITH_SCOPE("Invalid expandedKey size");
        }
        break;
    case MLDSA65:
        if (expanded_key.size() != 4032) {
            ERROR_WITH_SCOPE("Invalid expandedKey size");
        }
        break;
    case MLDSA87:
        if (expanded_key.size() != 4896) {
            ERROR_WITH_SCOPE("Invalid expandedKey size");
        }
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    POP_SCOPE();

    return ByteBuffer::copy(expanded_key);
}

ErrorOr<ByteBuffer> MLDSAPrivateKey::export_as_der() const
{
    ASN1::Encoder encoder;

    TRY(encoder.write<ReadonlyBytes>(m_seed, ASN1::Class::Context, static_cast<ASN1::Kind>(0)));

    return encoder.finish();
}

// https://www.rfc-editor.org/rfc/rfc9881.html#section-6
ErrorOr<MLDSA::KeyPairType> MLDSA::parse_mldsa_key(MLDSASize size, ReadonlyBytes der, Vector<StringView> current_scope)
{
    ASN1::Decoder decoder(der);

    // ML-DSA-PrivateKey ::= CHOICE {
    //      seed [0] IMPLICIT OCTET STRING (SIZE (32)),
    //      expandedKey OCTET STRING (SIZE (2560 | 4032 | 4896)),
    //      both SEQUENCE {
    //           seed OCTET STRING (SIZE (32)),
    //           expandedKey OCTET STRING (SIZE (2560 | 4032 | 4896))
    //      }
    // }

    if (decoder.eof()) {
        return Error::from_string_literal("Input key is empty");
    }

    auto const tag = TRY(decoder.peek());
    if (static_cast<u8>(tag.kind) == 0) {
        REWRITE_TAG(OctetString);
        return generate_key_pair(size, TRY(read_mldsa_seed(decoder, current_scope)));
    }
    if (tag.kind == ASN1::Kind::OctetString) {
        return KeyPairType {
            {},
            { {}, {}, TRY(read_mldsa_private_key(size, decoder, current_scope)) }
        };
    }
    if (tag.kind == ASN1::Kind::Sequence) {
        ENTER_TYPED_SCOPE(Sequence, "both");
        ENTER_TYPED_SCOPE(OctetString, "seed");
        auto key_pair = TRY(generate_key_pair(size, TRY(read_mldsa_seed(decoder, current_scope))));
        POP_SCOPE()

        ENTER_TYPED_SCOPE(OctetString, "expandedKey");
        if (auto const expanded_key = TRY(read_mldsa_private_key(size, decoder, current_scope));
            key_pair.private_key.private_key() != expanded_key) {
            ERROR_WITH_SCOPE("Invalid expanded_key");
        }
        POP_SCOPE();

        POP_SCOPE();
        return key_pair;
    }

    return Error::from_string_literal("Invalid key format");
}

ErrorOr<MLDSA::KeyPairType> MLDSA::generate_key_pair(MLDSASize size, ByteBuffer seed)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, mldsa_size_to_openssl_name(size), nullptr)));

    OPENSSL_TRY(EVP_PKEY_keygen_init(ctx.ptr()));

    OSSL_PARAM params[2] = {
        OSSL_PARAM_END,
        OSSL_PARAM_END
    };

    if (!seed.is_empty()) {
        params[0] = OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ML_DSA_SEED, seed.data(), seed.size());
    }

    OPENSSL_TRY(EVP_PKEY_CTX_set_params(ctx.ptr(), params));
    auto key = TRY(OpenSSL_PKEY::create());
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_generate(ctx.ptr(), &key_ptr));

    // We reserve enough memory for the key size to be able to fit them all
    auto pub = TRY(ByteBuffer::create_uninitialized(2592));
    auto priv = TRY(ByteBuffer::create_uninitialized(4896));
    seed = TRY(ByteBuffer::create_uninitialized(32));

    size_t priv_len, pub_len, seed_len;
    OPENSSL_TRY(EVP_PKEY_get_octet_string_param(key.ptr(), OSSL_PKEY_PARAM_ML_DSA_SEED, seed.data(), seed.size(), &seed_len));
    OPENSSL_TRY(EVP_PKEY_get_octet_string_param(key.ptr(), OSSL_PKEY_PARAM_PRIV_KEY, priv.data(), priv.size(), &priv_len));
    OPENSSL_TRY(EVP_PKEY_get_octet_string_param(key.ptr(), OSSL_PKEY_PARAM_PUB_KEY, pub.data(), pub.size(), &pub_len));

    pub.trim(pub_len, true);
    priv.trim(priv_len, true);

    return KeyPairType {
        { pub },
        { seed, pub, priv }
    };
}

static ErrorOr<OpenSSL_PKEY> private_key_to_openssl_pkey(MLDSASize size, MLDSAPrivateKey const& private_key)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, mldsa_size_to_openssl_name(size), nullptr)));

    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx.ptr()));

    auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_ML_DSA_SEED, private_key.seed().data(), private_key.seed().size()));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_PUB_KEY, private_key.public_key().data(), private_key.public_key().size()));
    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_PRIV_KEY, private_key.private_key().data(), private_key.private_key().size()));

    auto* params = OSSL_PARAM_BLD_to_param(params_bld);
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto key = TRY(OpenSSL_PKEY::create());
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx.ptr(), &key_ptr, EVP_PKEY_KEYPAIR, params));

    return key;
}

ErrorOr<ByteBuffer> MLDSA::sign(ReadonlyBytes message)
{
    OSSL_PARAM params[2] = {
        OSSL_PARAM_END,
        OSSL_PARAM_END
    };

    if (!m_context.is_empty()) {
        params[0] = OSSL_PARAM_octet_string(OSSL_SIGNATURE_PARAM_CONTEXT_STRING, m_context.data(), m_context.size());
    }

    auto key = TRY(private_key_to_openssl_pkey(m_size, m_private_key));
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, mldsa_size_to_openssl_name(m_size), nullptr)));

    EVP_PKEY_CTX* sign_ctx = OPENSSL_TRY_PTR(EVP_PKEY_CTX_new_from_pkey(nullptr, key.ptr(), nullptr));
    ScopeGuard const free_sign_ctx = [&] { EVP_PKEY_CTX_free(sign_ctx); };
    auto* sign_algorithm = OPENSSL_TRY_PTR(EVP_SIGNATURE_fetch(nullptr, mldsa_size_to_openssl_name(m_size), nullptr));
    ScopeGuard const free_sign_algorithm = [&] { EVP_SIGNATURE_free(sign_algorithm); };

    size_t sign_size;
    OPENSSL_TRY(EVP_PKEY_sign_message_init(sign_ctx, sign_algorithm, params));
    OPENSSL_TRY(EVP_PKEY_sign(sign_ctx, nullptr, &sign_size, message.data(), message.size()));

    auto result = TRY(ByteBuffer::create_uninitialized(sign_size));
    OPENSSL_TRY(EVP_PKEY_sign(sign_ctx, result.data(), &sign_size, message.data(), message.size()));

    return result;
}

static ErrorOr<OpenSSL_PKEY> public_key_to_openssl_pkey(MLDSASize size, MLDSAPublicKey const& public_key)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, mldsa_size_to_openssl_name(size), nullptr)));

    OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx.ptr()));

    auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
    ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

    OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_PUB_KEY, public_key.public_key().data(), public_key.public_key().size()));

    auto* params = OSSL_PARAM_BLD_to_param(params_bld);
    ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

    auto key = TRY(OpenSSL_PKEY::create());
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_fromdata(ctx.ptr(), &key_ptr, EVP_PKEY_KEYPAIR, params));

    return key;
}

ErrorOr<bool> MLDSA::verify(ReadonlyBytes message, ReadonlyBytes signature)
{
    OSSL_PARAM params[2] = {
        OSSL_PARAM_END,
        OSSL_PARAM_END
    };

    if (!m_context.is_empty()) {
        params[0] = OSSL_PARAM_octet_string(OSSL_SIGNATURE_PARAM_CONTEXT_STRING, m_context.data(), m_context.size());
    }

    auto key = TRY(public_key_to_openssl_pkey(m_size, m_public_key));
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new(key.ptr(), nullptr)));
    auto* sign_algorithm = OPENSSL_TRY_PTR(EVP_SIGNATURE_fetch(nullptr, mldsa_size_to_openssl_name(m_size), nullptr));
    ScopeGuard const free_sign_algorithm = [&] { EVP_SIGNATURE_free(sign_algorithm); };

    OPENSSL_TRY(EVP_PKEY_verify_message_init(ctx.ptr(), sign_algorithm, params));

    auto ret = EVP_PKEY_verify(ctx.ptr(), signature.data(), signature.size(), message.data(), message.size());
    if (ret == 1)
        return true;
    if (ret == 0)
        return false;
    OPENSSL_TRY(ret);
    VERIFY_NOT_REACHED();
}

}
