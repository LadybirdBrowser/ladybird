/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <AK/Types.h>
#include <LibCrypto/Hash/MD5.h>

static constexpr u32 F(u32 x, u32 y, u32 z) { return (x & y) | ((~x) & z); }
static constexpr u32 G(u32 x, u32 y, u32 z) { return (x & z) | ((~z) & y); }
static constexpr u32 H(u32 x, u32 y, u32 z) { return x ^ y ^ z; }
static constexpr u32 I(u32 x, u32 y, u32 z) { return y ^ (x | ~z); }
static constexpr u32 ROTATE_LEFT(u32 x, size_t n)
{
    return (x << n) | (x >> (32 - n));
}

static constexpr void round_1(u32& a, u32 b, u32 c, u32 d, u32 x, u32 s, u32 ac)
{
    a += F(b, c, d) + x + ac;
    a = ROTATE_LEFT(a, s);
    a += b;
}

static constexpr void round_2(u32& a, u32 b, u32 c, u32 d, u32 x, u32 s, u32 ac)
{
    a += G(b, c, d) + x + ac;
    a = ROTATE_LEFT(a, s);
    a += b;
}

static constexpr void round_3(u32& a, u32 b, u32 c, u32 d, u32 x, u32 s, u32 ac)
{
    a += H(b, c, d) + x + ac;
    a = ROTATE_LEFT(a, s);
    a += b;
}

static constexpr void round_4(u32& a, u32 b, u32 c, u32 d, u32 x, u32 s, u32 ac)
{
    a += I(b, c, d) + x + ac;
    a = ROTATE_LEFT(a, s);
    a += b;
}

namespace Crypto::Hash {

void MD5::update(u8 const* input, size_t length)
{
    auto index = (u32)(m_count[0] >> 3) & 0x3f;
    size_t offset { 0 };
    m_count[0] += (u32)length << 3;
    if (m_count[0] < ((u32)length << 3)) {
        ++m_count[1];
    }
    m_count[1] += (u32)length >> 29;

    auto part_length = 64 - index;
    auto buffer = Bytes { m_data_buffer, sizeof(m_data_buffer) };
    if (length >= part_length) {
        buffer.overwrite(index, input, part_length);
        transform(buffer.data());

        for (offset = part_length; offset + 63 < length; offset += 64)
            transform(&input[offset]);

        index = 0;
    }

    VERIFY(length < part_length || length - offset <= 64);
    buffer.overwrite(index, &input[offset], length - offset);
}
MD5::DigestType MD5::digest()
{
    auto digest = peek();
    reset();
    return digest;
}

MD5::DigestType MD5::peek()
{
    DigestType digest;
    u8 bits[8];

    encode(m_count, bits, 8);

    // pad the data to 56%64
    u32 index = (u32)((m_count[0] >> 3) & 0x3f);
    u32 pad_length = index < 56 ? 56 - index : 120 - index;
    update(MD5Constants::PADDING, pad_length);

    // append length
    update(bits, 8);

    // store state (4 registers ABCD)
    encode(&m_A, digest.data, 4 * sizeof(m_A));

    return digest;
}

void MD5::encode(u32 const* from, u8* to, size_t length)
{
    for (size_t i = 0, j = 0; j < length; ++i, j += 4) {
        to[j] = (u8)(from[i] & 0xff);
        to[j + 1] = (u8)((from[i] >> 8) & 0xff);
        to[j + 2] = (u8)((from[i] >> 16) & 0xff);
        to[j + 3] = (u8)((from[i] >> 24) & 0xff);
    }
}

void MD5::decode(u8 const* from, u32* to, size_t length)
{
    for (size_t i = 0, j = 0; j < length; ++i, j += 4)
        to[i] = (((u32)from[j]) | (((u32)from[j + 1]) << 8) | (((u32)from[j + 2]) << 16) | (((u32)from[j + 3]) << 24));
}

void MD5::transform(u8 const* block)
{
    auto a = m_A;
    auto b = m_B;
    auto c = m_C;
    auto d = m_D;
    u32 x[16];

    decode(block, x, 64);

    round_1(a, b, c, d, x[0], MD5Constants::S11, 0xd76aa478);  // 1
    round_1(d, a, b, c, x[1], MD5Constants::S12, 0xe8c7b756);  // 2
    round_1(c, d, a, b, x[2], MD5Constants::S13, 0x242070db);  // 3
    round_1(b, c, d, a, x[3], MD5Constants::S14, 0xc1bdceee);  // 4
    round_1(a, b, c, d, x[4], MD5Constants::S11, 0xf57c0faf);  // 5
    round_1(d, a, b, c, x[5], MD5Constants::S12, 0x4787c62a);  // 6
    round_1(c, d, a, b, x[6], MD5Constants::S13, 0xa8304613);  // 7
    round_1(b, c, d, a, x[7], MD5Constants::S14, 0xfd469501);  // 8
    round_1(a, b, c, d, x[8], MD5Constants::S11, 0x698098d8);  // 9
    round_1(d, a, b, c, x[9], MD5Constants::S12, 0x8b44f7af);  // 10
    round_1(c, d, a, b, x[10], MD5Constants::S13, 0xffff5bb1); // 11
    round_1(b, c, d, a, x[11], MD5Constants::S14, 0x895cd7be); // 12
    round_1(a, b, c, d, x[12], MD5Constants::S11, 0x6b901122); // 13
    round_1(d, a, b, c, x[13], MD5Constants::S12, 0xfd987193); // 14
    round_1(c, d, a, b, x[14], MD5Constants::S13, 0xa679438e); // 15
    round_1(b, c, d, a, x[15], MD5Constants::S14, 0x49b40821); // 16

    round_2(a, b, c, d, x[1], MD5Constants::S21, 0xf61e2562);  // 17
    round_2(d, a, b, c, x[6], MD5Constants::S22, 0xc040b340);  // 18
    round_2(c, d, a, b, x[11], MD5Constants::S23, 0x265e5a51); // 19
    round_2(b, c, d, a, x[0], MD5Constants::S24, 0xe9b6c7aa);  // 20
    round_2(a, b, c, d, x[5], MD5Constants::S21, 0xd62f105d);  // 21
    round_2(d, a, b, c, x[10], MD5Constants::S22, 0x2441453);  // 22
    round_2(c, d, a, b, x[15], MD5Constants::S23, 0xd8a1e681); // 23
    round_2(b, c, d, a, x[4], MD5Constants::S24, 0xe7d3fbc8);  // 24
    round_2(a, b, c, d, x[9], MD5Constants::S21, 0x21e1cde6);  // 25
    round_2(d, a, b, c, x[14], MD5Constants::S22, 0xc33707d6); // 26
    round_2(c, d, a, b, x[3], MD5Constants::S23, 0xf4d50d87);  // 27
    round_2(b, c, d, a, x[8], MD5Constants::S24, 0x455a14ed);  // 28
    round_2(a, b, c, d, x[13], MD5Constants::S21, 0xa9e3e905); // 29
    round_2(d, a, b, c, x[2], MD5Constants::S22, 0xfcefa3f8);  // 30
    round_2(c, d, a, b, x[7], MD5Constants::S23, 0x676f02d9);  // 31
    round_2(b, c, d, a, x[12], MD5Constants::S24, 0x8d2a4c8a); // 32

    round_3(a, b, c, d, x[5], MD5Constants::S31, 0xfffa3942);  // 33
    round_3(d, a, b, c, x[8], MD5Constants::S32, 0x8771f681);  // 34
    round_3(c, d, a, b, x[11], MD5Constants::S33, 0x6d9d6122); // 35
    round_3(b, c, d, a, x[14], MD5Constants::S34, 0xfde5380c); // 36
    round_3(a, b, c, d, x[1], MD5Constants::S31, 0xa4beea44);  // 37
    round_3(d, a, b, c, x[4], MD5Constants::S32, 0x4bdecfa9);  // 38
    round_3(c, d, a, b, x[7], MD5Constants::S33, 0xf6bb4b60);  // 39
    round_3(b, c, d, a, x[10], MD5Constants::S34, 0xbebfbc70); // 40
    round_3(a, b, c, d, x[13], MD5Constants::S31, 0x289b7ec6); // 41
    round_3(d, a, b, c, x[0], MD5Constants::S32, 0xeaa127fa);  // 42
    round_3(c, d, a, b, x[3], MD5Constants::S33, 0xd4ef3085);  // 43
    round_3(b, c, d, a, x[6], MD5Constants::S34, 0x4881d05);   // 44
    round_3(a, b, c, d, x[9], MD5Constants::S31, 0xd9d4d039);  // 45
    round_3(d, a, b, c, x[12], MD5Constants::S32, 0xe6db99e5); // 46
    round_3(c, d, a, b, x[15], MD5Constants::S33, 0x1fa27cf8); // 47
    round_3(b, c, d, a, x[2], MD5Constants::S34, 0xc4ac5665);  // 48

    round_4(a, b, c, d, x[0], MD5Constants::S41, 0xf4292244);  // 49
    round_4(d, a, b, c, x[7], MD5Constants::S42, 0x432aff97);  // 50
    round_4(c, d, a, b, x[14], MD5Constants::S43, 0xab9423a7); // 51
    round_4(b, c, d, a, x[5], MD5Constants::S44, 0xfc93a039);  // 52
    round_4(a, b, c, d, x[12], MD5Constants::S41, 0x655b59c3); // 53
    round_4(d, a, b, c, x[3], MD5Constants::S42, 0x8f0ccc92);  // 54
    round_4(c, d, a, b, x[10], MD5Constants::S43, 0xffeff47d); // 55
    round_4(b, c, d, a, x[1], MD5Constants::S44, 0x85845dd1);  // 56
    round_4(a, b, c, d, x[8], MD5Constants::S41, 0x6fa87e4f);  // 57
    round_4(d, a, b, c, x[15], MD5Constants::S42, 0xfe2ce6e0); // 58
    round_4(c, d, a, b, x[6], MD5Constants::S43, 0xa3014314);  // 59
    round_4(b, c, d, a, x[13], MD5Constants::S44, 0x4e0811a1); // 60
    round_4(a, b, c, d, x[4], MD5Constants::S41, 0xf7537e82);  // 61
    round_4(d, a, b, c, x[11], MD5Constants::S42, 0xbd3af235); // 62
    round_4(c, d, a, b, x[2], MD5Constants::S43, 0x2ad7d2bb);  // 63
    round_4(b, c, d, a, x[9], MD5Constants::S44, 0xeb86d391);  // 64

    m_A += a;
    m_B += b;
    m_C += c;
    m_D += d;

    secure_zero(x, sizeof(x));
}

}
