/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>

namespace Crypto::Hash {

// https://www.rfc-editor.org/rfc/rfc2898#section-5.2
class PBKDF2 {
public:
    template<typename PRF>
    static ErrorOr<ByteBuffer> derive_key(ReadonlyBytes password, ReadonlyBytes salt, u32 iterations, u32 key_length_bytes)
    requires requires(PRF t) {
        t.digest_size();
    }
    {
        PRF prf(password);

        // Note: hLen denotes the length in octets of the pseudorandom function output
        u32 h_len = prf.digest_size();

        // 1. If dkLen > (2^32 - 1) * hLen, output "derived key too long" and stop.
        if (key_length_bytes > (AK::pow(2.0, 32.0) - 1) * h_len)
            return Error::from_string_literal("derived key too long");

        // 2 . Let l be the number of hLen-octet blocks in the derived key rounding up,
        //     and let r be the number of octets in the last block
        u32 l = AK::ceil_div(key_length_bytes, h_len);
        u32 r = key_length_bytes - (l - 1) * h_len;

        // 3. For each block of the derived key apply the function F defined
        //    below to the password P, the salt S, the iteration count c, and
        //    the block index to compute the block:

        ByteBuffer ui = TRY(ByteBuffer::create_zeroed(h_len));
        ByteBuffer ti = TRY(ByteBuffer::create_zeroed(h_len));
        ByteBuffer key = TRY(ByteBuffer::create_zeroed(key_length_bytes));

        // T_i = F (P, S, c, i)
        u8 iteration_bytes[4];
        for (u32 i = 1; i <= l; i++) {
            iteration_bytes[3] = i;
            iteration_bytes[2] = ((i >> 8) & 0xff);
            iteration_bytes[1] = ((i >> 16) & 0xff);
            iteration_bytes[0] = ((i >> 24) & 0xff);

            prf.update(salt);
            prf.update(ReadonlyBytes { iteration_bytes, 4 });
            auto digest = prf.digest();
            ui.overwrite(0, digest.immutable_data(), h_len);
            ti.overwrite(0, digest.immutable_data(), h_len);

            // U_1 = PRF (P, S || INT (i))
            // U_j = PRF (P, U_{j-1})

            // F (P, S, c, i) = U_1 \xor U_2 \xor ... \xor U_c
            for (u32 j = 2; j <= iterations; j++) {
                prf.update(ui.bytes());
                auto digest_inner = prf.digest();
                ui.overwrite(0, digest_inner.immutable_data(), h_len);

                UnsignedBigInteger ti_temp = UnsignedBigInteger::import_data(ti.data(), ti.size());
                UnsignedBigInteger ui_temp = UnsignedBigInteger::import_data(ui.data(), ui.size());
                UnsignedBigInteger r_temp = ti_temp.bitwise_xor(ui_temp);

                r_temp.export_data(ti.bytes());
            }

            //  4. Concatenate the blocks and extract the first dkLen octets to produce a derived key DK:
            key.overwrite((i - 1) * h_len, ti.data(), i == l ? r : h_len);
        }

        // 5. Output the derived key DK
        return key;
    }
};

}
