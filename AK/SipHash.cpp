/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/Random.h>
#include <AK/Singleton.h>
#include <AK/SipHash.h>
#include <AK/Span.h>
#include <AK/UFixedBigInt.h>

namespace AK {

ALWAYS_INLINE constexpr u64 rotate_left(u64 x, u64 bits)
{
    return static_cast<u64>(((x) << (bits)) | ((x) >> (64 - (bits))));
}

ALWAYS_INLINE constexpr void sipround(u64& v0, u64& v1, u64& v2, u64& v3)
{
    v0 += v1;
    v1 = rotate_left(v1, 13);
    v1 ^= v0;
    v0 = rotate_left(v0, 32);
    v2 += v3;
    v3 = rotate_left(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = rotate_left(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = rotate_left(v1, 17);
    v1 ^= v2;
    v2 = rotate_left(v2, 32);
}

// Can handle u64 or u128 output as per reference implementation.
// We currenly only use u64 and further fold it to u32 (unsigned) for use in Traits.
template<size_t message_block_rounds, size_t finalization_rounds>
static void do_siphash(ReadonlyBytes input, u128 key, Bytes output)
{
    VERIFY((output.size() == 8) || (output.size() == 16));

    u64 v0 = 0x736f6d6570736575ull;
    u64 v1 = 0x646f72616e646f6dull;
    u64 v2 = 0x6c7967656e657261ull;
    u64 v3 = 0x7465646279746573ull;
    u64 const length = input.size();
    auto const left = length & 7;
    // The end of 64-bit blocks.
    auto const block_end = length - (length % sizeof(u64));
    u64 b = length << 56;
    v3 ^= key.high();
    v2 ^= key.low();
    v1 ^= key.high();
    v0 ^= key.low();

    if (output.size() == 16)
        v1 ^= 0xee;

    for (size_t input_index = 0; input_index < block_end; input_index += 8) {
        u64 const m = bit_cast<LittleEndian<u64>>(ByteReader::load64(input.slice(input_index, sizeof(u64)).data()));
        v3 ^= m;

        for (size_t i = 0; i < message_block_rounds; ++i)
            sipround(v0, v1, v2, v3);

        v0 ^= m;
    }

    switch (left) {
    case 7:
        b |= (static_cast<u64>(input[block_end + 6])) << 48;
        [[fallthrough]];
    case 6:
        b |= (static_cast<u64>(input[block_end + 5])) << 40;
        [[fallthrough]];
    case 5:
        b |= (static_cast<u64>(input[block_end + 4])) << 32;
        [[fallthrough]];
    case 4:
        b |= (static_cast<u64>(input[block_end + 3])) << 24;
        [[fallthrough]];
    case 3:
        b |= (static_cast<u64>(input[block_end + 2])) << 16;
        [[fallthrough]];
    case 2:
        b |= (static_cast<u64>(input[block_end + 1])) << 8;
        [[fallthrough]];
    case 1:
        b |= (static_cast<u64>(input[block_end + 0]));
        break;
    case 0:
        break;
    }

    v3 ^= b;

    for (size_t i = 0; i < message_block_rounds; ++i)
        sipround(v0, v1, v2, v3);

    v0 ^= b;

    if (output.size() == 16)
        v2 ^= 0xee;
    else
        v2 ^= 0xff;

    for (size_t i = 0; i < finalization_rounds; ++i)
        sipround(v0, v1, v2, v3);

    b = v0 ^ v1 ^ v2 ^ v3;

    LittleEndian<u64> b_le { b };
    output.overwrite(0, &b_le, sizeof(b_le));

    if (output.size() == 8)
        return;

    v1 ^= 0xdd;

    for (size_t i = 0; i < finalization_rounds; ++i)
        sipround(v0, v1, v2, v3);

    b = v0 ^ v1 ^ v2 ^ v3;
    b_le = b;
    output.overwrite(sizeof(b_le), &b_le, sizeof(b_le));
}

struct SipHashKey {
    SipHashKey()
    {
        // get_random is assumed to be secure, otherwise SipHash doesn't deliver on its promises!
        key = get_random<u128>();
    }
    constexpr u128 operator*() const { return key; }
    u128 key;
};
// Using a singleton is a little heavier than a plain static, but avoids an initialization order fiasco.
static Singleton<SipHashKey> static_sip_hash_key;

template<size_t message_block_rounds, size_t finalization_rounds>
unsigned sip_hash_u64(u64 input)
{
    ReadonlyBytes input_bytes { &input, sizeof(input) };
    u64 const output_u64 = sip_hash_bytes<message_block_rounds, finalization_rounds>(input_bytes);
    return static_cast<unsigned>(output_u64 ^ (output_u64 >> 32));
}

unsigned standard_sip_hash(u64 input)
{
    return sip_hash_u64<1, 3>(input);
}

unsigned secure_sip_hash(u64 input)
{
    return sip_hash_u64<4, 8>(input);
}

template<size_t message_block_rounds, size_t finalization_rounds>
u64 sip_hash_bytes(ReadonlyBytes input)
{
    auto sip_hash_key = **static_sip_hash_key;
    u64 output = 0;
    Bytes output_bytes { &output, sizeof(output) };
    do_siphash<message_block_rounds, finalization_rounds>(input, sip_hash_key, output_bytes);
    return output;
}

// Instantiate all used SipHash variants here:
template u64 sip_hash_bytes<1, 3>(ReadonlyBytes);
template u64 sip_hash_bytes<4, 8>(ReadonlyBytes);

}
