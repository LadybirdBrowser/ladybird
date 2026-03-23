/*
 * Copyright (c) 2026, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Authentication/KMAC.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>

namespace Crypto::Authentication {

Optional<KMACKind> KMAC::kind_from_algorithm_name(StringView algorithm_name)
{
    if (algorithm_name == "KMAC128"sv)
        return KMACKind::KMAC128;
    if (algorithm_name == "KMAC256"sv)
        return KMACKind::KMAC256;
    return {};
}

u32 KMAC::default_key_length(KMACKind kind)
{
    switch (kind) {
    case KMACKind::KMAC128:
        return 128;
    case KMACKind::KMAC256:
        return 256;
    }
    VERIFY_NOT_REACHED();
}

KMAC::KMAC(KMACKind kind)
    : m_kind(kind)
{
}

// https://wicg.github.io/webcrypto-modern-algos/#kmac-operations-sign
ErrorOr<ByteBuffer> KMAC::sign(ReadonlyBytes key, ReadonlyBytes message, u32 output_length_bits, Optional<ReadonlyBytes> customization) const
{
    if (output_length_bits == 0)
        return Error::from_string_literal("KMAC output length must be greater than zero");

    if ((output_length_bits % 8) != 0)
        return Error::from_string_literal("KMAC output length must be a multiple of 8");

    auto const* mac_name = (m_kind == KMACKind::KMAC128) ? OSSL_MAC_NAME_KMAC128 : OSSL_MAC_NAME_KMAC256;

    auto mac = TRY(OpenSSL_MAC::wrap(EVP_MAC_fetch(nullptr, mac_name, nullptr)));
    auto ctx = TRY(OpenSSL_MAC_CTX::wrap(EVP_MAC_CTX_new(mac.ptr())));

    size_t output_size = output_length_bits / 8;

    Vector<OSSL_PARAM> params;
    params.append(OSSL_PARAM_size_t(OSSL_MAC_PARAM_SIZE, &output_size));
    if (customization.has_value())
        params.append(OSSL_PARAM_octet_string(OSSL_MAC_PARAM_CUSTOM, const_cast<u8*>(customization.value().data()), customization.value().size()));
    params.append(OSSL_PARAM_END);

    OPENSSL_TRY(EVP_MAC_init(ctx.ptr(), key.data(), key.size(), params.data()));
    OPENSSL_TRY(EVP_MAC_update(ctx.ptr(), message.data(), message.size()));

    auto buf = TRY(ByteBuffer::create_uninitialized(output_size));
    size_t written = 0;
    OPENSSL_TRY(EVP_MAC_final(ctx.ptr(), buf.data(), &written, output_size));

    if (written != output_size)
        return Error::from_string_literal("EVP_MAC_final returned an unexpected output length");

    return buf;
}

}
