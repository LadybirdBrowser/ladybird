/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteReader.h>
#include <AK/Endian.h>

namespace Crypto::Hash {

class MGF {
public:
    // https://datatracker.ietf.org/doc/html/rfc2437#section-10.2.1
    template<typename HashFunction>
    static ErrorOr<ByteBuffer> mgf1(ReadonlyBytes seed, size_t length)
    requires requires { HashFunction::digest_size(); }
    {
        HashFunction hash;

        size_t h_len = hash.digest_size();

        // 1. If length > 2^32(hLen), output "mask too long" and stop.
        if constexpr (sizeof(size_t) > 32) {
            if (length > (h_len << 32))
                return Error::from_string_literal("mask too long");
        }

        // 2. Let T be the empty octet string.
        auto t = TRY(ByteBuffer::create_uninitialized(0));

        // 3. For counter from 0 to ceil(length / hLen) - 1, do the following:
        auto counter = 0u;
        auto iterations = AK::ceil_div(length, h_len) - 1;

        auto c = TRY(ByteBuffer::create_uninitialized(4));
        for (; counter <= iterations; ++counter) {
            // a. Convert counter to an octet string C of length 4 with the primitive I2OSP: C = I2OSP(counter, 4)
            ByteReader::store(static_cast<u8*>(c.data()), AK::convert_between_host_and_big_endian(static_cast<u32>(counter)));

            // b. Concatenate the hash of the seed Z and C to the octet string T: T = T || Hash (Z || C)
            hash.update(seed);
            hash.update(c);
            auto digest = hash.digest();

            TRY(t.try_append(digest.bytes()));
        }

        // 4. Output the leading l octets of T as the octet string mask.
        return t.slice(0, length);
    }
};

}
