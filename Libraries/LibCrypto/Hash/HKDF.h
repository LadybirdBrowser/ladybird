/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/Authentication/HMAC.h>

namespace Crypto::Hash {

// https://www.rfc-editor.org/rfc/rfc5869#section-2
template<typename HashT>
class HKDF {
public:
    using HashType = HashT;
    using DigestType = typename HashType::DigestType;
    using HMACType = typename Crypto::Authentication::HMAC<HashType>;

    // Note: The output is different for a salt of length zero and an absent salt,
    // so Optional<ReadonlyBytes> really is the correct type.
    static ErrorOr<ByteBuffer> derive_key(Optional<ReadonlyBytes> maybe_salt, ReadonlyBytes input_keying_material, ReadonlyBytes info, u32 output_key_length)
    {
        if (output_key_length > 255 * DigestType::Size) {
            return Error::from_string_view("requested output_key_length is too large"sv);
        }
        // Note that it feels like we should also refuse to run with output_key_length == 0,
        // but the spec allows this.

        // https://www.rfc-editor.org/rfc/rfc5869#section-2.1
        // Note that in the extract step, 'IKM' is used as the HMAC input, not as the HMAC key.

        // salt: optional salt value (a non-secret random value); if not provided, it is set to a string of HashLen zeros.
        ByteBuffer salt_buffer;
        auto salt = maybe_salt.value_or_lazy_evaluated([&] {
            salt_buffer.resize(DigestType::Size, ByteBuffer::ZeroFillNewElements::Yes);
            return salt_buffer.bytes();
        });
        HMACType hmac_salt(salt);

        // https://www.rfc-editor.org/rfc/rfc5869#section-2.2
        // PRK = HMAC-Hash(salt, IKM)
        auto prk_digest = hmac_salt.process(input_keying_material);
        auto prk = prk_digest.bytes();
        ASSERT(prk.size() == DigestType::Size);

        // https://www.rfc-editor.org/rfc/rfc5869#section-2.3
        // N = ceil(L/HashLen)
        auto num_iterations = ceil_div(static_cast<size_t>(output_key_length), DigestType::Size);
        // T = T(1) | T(2) | T(3) | ... | T(N)
        ByteBuffer output_buffer;
        // where:
        // T(0) = empty string (zero length)
        // T(1) = HMAC-Hash(PRK, T(0) | info | 0x01)
        // T(2) = HMAC-Hash(PRK, T(1) | info | 0x02)
        // T(3) = HMAC-Hash(PRK, T(2) | info | 0x03)
        HMACType hmac_prk(prk);
        // In iteration i we compute T(i), and deduce T(i - 1) from 'output_buffer'.
        // Hence, we do not need to run i == 0.
        // INVARIANT: At the beginning of each iteration, hmac_prk is freshly reset.
        // For the first iteration, this is given by the constructor of HMAC.
        for (size_t i = 1; i < 1 + num_iterations; ++i) {
            if (i > 1) {
                auto t_i_minus_one = output_buffer.bytes().slice_from_end(DigestType::Size);
                hmac_prk.update(t_i_minus_one);
            }
            hmac_prk.update(info);
            u8 const pad_byte = static_cast<u8>(i & 0xff);
            hmac_prk.update(ReadonlyBytes(&pad_byte, 1));
            auto t_i_digest = hmac_prk.digest();
            output_buffer.append(t_i_digest.bytes());
        }

        // OKM = first L octets of T
        ASSERT(output_buffer.size() >= output_key_length);
        output_buffer.trim(output_key_length, false);

        // 5. Output the derived key DK
        return { output_buffer };
    }

private:
    HKDF() = delete;
};

}
