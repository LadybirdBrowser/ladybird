/*
 * Copyright (c) 2025, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCrypto/Hash/SHAKE.h>
#include <LibCrypto/OpenSSL.h>
#include <openssl/evp.h>

namespace Crypto::Hash {

SHAKE::SHAKE(SHAKEKind kind)
    : m_kind(kind)
{
}

ErrorOr<void> SHAKE::validate_parameters(u32 length)
{
    if (length % 8 != 0) {
        return Error::from_string_literal("SHAKE output length must be a multiple of 8 bits");
    }

    return {};
}

// https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-185.pdf
static ErrorOr<void> left_encode(ByteBuffer& output, u64 value)
{
    u8 bytes[sizeof(u64)];
    size_t length = 0;
    do {
        bytes[sizeof(u64) - ++length] = static_cast<u8>(value);
        value >>= 8;
    } while (value != 0);

    TRY(output.try_append(static_cast<u8>(length)));
    return output.try_append(bytes + sizeof(u64) - length, length);
}

// https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-185.pdf
static ErrorOr<void> encode_string(ByteBuffer& output, ReadonlyBytes string)
{
    TRY(left_encode(output, static_cast<u64>(string.size()) * 8));
    return output.try_append(string);
}

ErrorOr<ByteBuffer> SHAKE::digest(
    ReadonlyBytes data,
    u32 length,
    Optional<ReadonlyBytes> customization,
    Optional<ReadonlyBytes> function_name) const
{
    TRY(validate_parameters(length));

    size_t output_bytes = length / 8;
    auto buf = TRY(ByteBuffer::create_uninitialized(output_bytes));

    auto ctx = TRY(OpenSSL_MD_CTX::wrap(EVP_MD_CTX_new()));

    auto name = function_name.value_or({});
    auto customization_string = customization.value_or({});

    // If N = "" and S = "", return SHAKE128(X, L) (or SHAKE256(X, L) for cSHAKE256).
    if (name.is_empty() && customization_string.is_empty()) {
        OPENSSL_TRY(EVP_DigestInit_ex(ctx.ptr(), m_kind == SHAKEKind::CSHAKE128 ? EVP_shake128() : EVP_shake256(), nullptr));
        OPENSSL_TRY(EVP_DigestUpdate(ctx.ptr(), data.data(), data.size()));
        OPENSSL_TRY(EVP_DigestFinalXOF(ctx.ptr(), buf.data(), output_bytes));
        return buf;
    }

    // Else return KECCAK[256](bytepad(encode_string(N) || encode_string(S), 168) || X || 00, L) for cSHAKE128, or
    // KECCAK[512](bytepad(encode_string(N) || encode_string(S), 136) || X || 00, L) for cSHAKE256.
    // NB: OpenSSL's KECCAK-KMAC digests are KECCAK[c](M || 00, L), the function that KMAC builds on.
    size_t rate = m_kind == SHAKEKind::CSHAKE128 ? 168 : 136;
    auto const* keccak_name = m_kind == SHAKEKind::CSHAKE128 ? "KECCAK-KMAC-128" : "KECCAK-KMAC-256";

    ByteBuffer prefix;
    TRY(left_encode(prefix, rate));
    TRY(encode_string(prefix, name));
    TRY(encode_string(prefix, customization_string));
    TRY(prefix.try_resize(ceil_div(prefix.size(), rate) * rate, ByteBuffer::ZeroFillNewElements::Yes));

    auto* keccak = OPENSSL_TRY_PTR(EVP_MD_fetch(nullptr, keccak_name, nullptr));
    auto init_result = EVP_DigestInit_ex(ctx.ptr(), keccak, nullptr);
    EVP_MD_free(keccak);
    OPENSSL_TRY(init_result);

    OPENSSL_TRY(EVP_DigestUpdate(ctx.ptr(), prefix.data(), prefix.size()));
    OPENSSL_TRY(EVP_DigestUpdate(ctx.ptr(), data.data(), data.size()));
    OPENSSL_TRY(EVP_DigestFinalXOF(ctx.ptr(), buf.data(), output_bytes));
    return buf;
}

}
