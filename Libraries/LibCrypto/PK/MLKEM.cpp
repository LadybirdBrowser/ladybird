/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/PK/MLKEM.h>

#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Curves/SECPxxxr1.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace Crypto::PK {

static char const* mlkem_size_to_openssl_name(MLKEMSize size)
{
    switch (size) {
    case MLKEMSize::MLKEM512:
        return "ML-KEM-512";
    case MLKEMSize::MLKEM768:
        return "ML-KEM-768";
    case MLKEMSize::MLKEM1024:
        return "ML-KEM-1024";
    default:
        VERIFY_NOT_REACHED();
    }
}

static ErrorOr<ByteBuffer> read_mlkem_seed(ASN1::Decoder& decoder, Vector<StringView>& current_scope)
{
    // seed ::= OCTET STRING (SIZE (64))
    READ_OBJECT(OctetString, StringView, seed_bits);

    auto const seed = seed_bits.bytes();
    if (seed.size() != 64) {
        ERROR_WITH_SCOPE("Invalid seed length");
    }
    POP_SCOPE();

    return ByteBuffer::copy(seed);
}

static ErrorOr<ByteBuffer> read_mlkem_private_key(MLKEMSize size, ASN1::Decoder& decoder, Vector<StringView>& current_scope)
{
    // expandedKey ::= OCTET STRING (SIZE (1632 | 2400 | 3168))

    ENTER_TYPED_SCOPE(OctetString, "expandedKey");
    READ_OBJECT(OctetString, StringView, expanded_key_bits);

    auto const expanded_key = expanded_key_bits.bytes();
    switch (size) {
    case MLKEMSize::MLKEM512:
        if (expanded_key.size() != 1632) {
            ERROR_WITH_SCOPE("Invalid expandedKey size");
        }
        break;
    case MLKEMSize::MLKEM768:
        if (expanded_key.size() != 2400) {
            ERROR_WITH_SCOPE("Invalid expandedKey size");
        }
        break;
    case MLKEMSize::MLKEM1024:
        if (expanded_key.size() != 3168) {
            ERROR_WITH_SCOPE("Invalid expandedKey size");
        }
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    POP_SCOPE();

    return ByteBuffer::copy(expanded_key);
}

ErrorOr<ByteBuffer> MLKEMPrivateKey::export_as_der() const
{
    ASN1::Encoder encoder;

    TRY(encoder.write<ReadonlyBytes>(m_seed, ASN1::Class::Context, static_cast<ASN1::Kind>(0)));

    return encoder.finish();
}

// https://datatracker.ietf.org/doc/html/draft-ietf-lamps-kyber-certificates-11#autoid-7
ErrorOr<MLKEM::KeyPairType> MLKEM::parse_mlkem_key(MLKEMSize size, ReadonlyBytes der, Vector<StringView> current_scope)
{
    ASN1::Decoder decoder(der);

    // ML-KEM-PrivateKey ::= CHOICE {
    //      seed [0] IMPLICIT OCTET STRING (SIZE (64)),
    //      expandedKey OCTET STRING (SIZE (1632 | 2400 | 3168)),
    //      both SEQUENCE {
    //           seed OCTET STRING (SIZE (64)),
    //           expandedKey OCTET STRING (SIZE (1632 | 2400 | 3168))
    //      }
    // }

    if (decoder.eof()) {
        return Error::from_string_literal("Input key is empty");
    }

    auto const tag = TRY(decoder.peek());
    if (static_cast<u8>(tag.kind) == 0) {
        REWRITE_TAG(OctetString);
        return generate_key_pair(size, TRY(read_mlkem_seed(decoder, current_scope)));
    }
    if (tag.kind == ASN1::Kind::OctetString) {
        return KeyPairType {
            {},
            { {}, {}, TRY(read_mlkem_private_key(size, decoder, current_scope)) }
        };
    }
    if (tag.kind == ASN1::Kind::Sequence) {
        ENTER_TYPED_SCOPE(Sequence, "both");
        ENTER_TYPED_SCOPE(OctetString, "seed");
        auto key_pair = TRY(generate_key_pair(size, TRY(read_mlkem_seed(decoder, current_scope))));
        POP_SCOPE()

        ENTER_TYPED_SCOPE(OctetString, "expandedKey");
        if (auto const expanded_key = TRY(read_mlkem_private_key(size, decoder, current_scope));
            key_pair.private_key.private_key() != expanded_key) {
            ERROR_WITH_SCOPE("Invalid expanded_key");
        }
        POP_SCOPE();

        POP_SCOPE();
        return key_pair;
    }

    return Error::from_string_literal("Invalid key format");
}

ErrorOr<MLKEMEncapsulation> MLKEM::encapsulate(MLKEMSize size, MLKEMPublicKey const& key)
{
    auto public_key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new_raw_public_key_ex(nullptr, mlkem_size_to_openssl_name(size), nullptr, key.public_key().data(), key.public_key().size())));

    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, public_key.ptr(), nullptr)));

    OPENSSL_TRY(EVP_PKEY_encapsulate_init(ctx.ptr(), nullptr));

    size_t shared_key_size;
    size_t ciphertext_length;
    OPENSSL_TRY(EVP_PKEY_encapsulate(ctx.ptr(), nullptr, &ciphertext_length, nullptr, &shared_key_size));

    auto shared_key = TRY(ByteBuffer::create_uninitialized(shared_key_size));
    auto ciphertext = TRY(ByteBuffer::create_uninitialized(ciphertext_length));

    OPENSSL_TRY(EVP_PKEY_encapsulate(ctx.ptr(), ciphertext.data(), &ciphertext_length, shared_key.data(), &shared_key_size));

    return MLKEMEncapsulation { shared_key, ciphertext };
}

ErrorOr<MLKEM::KeyPairType> MLKEM::generate_key_pair(MLKEMSize size, ByteBuffer seed)
{
    auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, mlkem_size_to_openssl_name(size), nullptr)));

    OPENSSL_TRY(EVP_PKEY_keygen_init(ctx.ptr()));

    OSSL_PARAM params[2] = {
        OSSL_PARAM_END,
        OSSL_PARAM_END
    };

    if (!seed.is_empty()) {
        params[0] = OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ML_KEM_SEED, seed.data(), seed.size());
    }

    OPENSSL_TRY(EVP_PKEY_CTX_set_params(ctx.ptr(), params));
    auto key = TRY(OpenSSL_PKEY::create());
    auto* key_ptr = key.ptr();
    OPENSSL_TRY(EVP_PKEY_generate(ctx.ptr(), &key_ptr));

    auto pub = TRY(get_byte_buffer_param_from_key(key, OSSL_PKEY_PARAM_PUB_KEY));
    auto priv = TRY(get_byte_buffer_param_from_key(key, OSSL_PKEY_PARAM_PRIV_KEY));
    seed = TRY(get_byte_buffer_param_from_key(key, OSSL_PKEY_PARAM_ML_KEM_SEED));

    return KeyPairType {
        MLKEMPublicKey { pub },
        { seed, pub, priv }
    };
}

}
