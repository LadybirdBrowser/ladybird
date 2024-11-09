/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteReader.h>
#include <AK/ByteString.h>
#include <AK/Endian.h>
#include <AK/Types.h>
#include <LibCrypto/Hash/HashFunction.h>

namespace Crypto::Authentication {

void galois_multiply(u32 (&z)[4], u32 const (&x)[4], u32 const (&y)[4]);

struct GHashDigest {
    constexpr static size_t Size = 16;
    u8 data[Size];

    u8 const* immutable_data() const { return data; }
    size_t data_length() { return Size; }
};

class GHash final {
public:
    using TagType = GHashDigest;

    template<size_t N>
    explicit GHash(char const (&key)[N])
        : GHash({ key, N })
    {
    }

    explicit GHash(ReadonlyBytes key)
    {
        VERIFY(key.size() >= 16);
        for (size_t i = 0; i < 16; i += 4) {
            m_key[i / 4] = AK::convert_between_host_and_big_endian(ByteReader::load32(key.offset(i)));
        }
    }

    constexpr static size_t digest_size() { return TagType::Size; }

    ByteString class_name() const
    {
        return "GHash";
    }

    TagType process(ReadonlyBytes aad, ReadonlyBytes cipher);

private:
    u32 m_key[4];
};

}
