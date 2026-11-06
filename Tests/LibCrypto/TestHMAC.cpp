/*
 * Copyright (c) 2021-present, the Ladybird developers.
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Authentication/HMAC.h>
#include <LibTest/TestCase.h>

static ByteBuffer operator""_b(char const* string, size_t length)
{
    return MUST(ByteBuffer::copy(string, length));
}

TEST_CASE(test_hmac_md5_process)
{
    auto key = "Well Hello Friends"_b;
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::MD5, key);
    u8 result[] {
        0x3b, 0x5b, 0xde, 0x30, 0x3a, 0x54, 0x7b, 0xbb, 0x09, 0xfe, 0x78, 0x89, 0xbc, 0x9f, 0x22, 0xa3
    };
    auto mac = hmac.process("Some bogus data"sv);
    EXPECT(memcmp(result, mac.data(), hmac.digest_size()) == 0);
}

TEST_CASE(test_hmac_sha256_process)
{
    auto key = "Well Hello Friends"_b;
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::SHA256, key);
    u8 result[] {
        0x1a, 0xf2, 0x20, 0x62, 0xde, 0x3b, 0x84, 0x65, 0xc1, 0x25, 0x23, 0x99, 0x76, 0x15, 0x1b, 0xec, 0x15, 0x21, 0x82, 0x1f, 0x23, 0xca, 0x11, 0x66, 0xdd, 0x8c, 0x6e, 0xf1, 0x81, 0x3b, 0x7f, 0x1b
    };
    auto mac = hmac.process("Some bogus data"sv);
    EXPECT(memcmp(result, mac.data(), hmac.digest_size()) == 0);
}

TEST_CASE(test_hmac_sha256_reuse)
{
    auto key = "Well Hello Friends"_b;
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::SHA256, key);

    auto mac_0 = hmac.process("Some bogus data"sv);
    auto mac_1 = hmac.process("Some bogus data"sv);

    EXPECT(memcmp(mac_0.data(), mac_1.data(), hmac.digest_size()) == 0);
}

TEST_CASE(test_hmac_sha256_data_is_same_size_as_block)
{
    auto key = "Well Hello Friends"_b;
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::SHA256, key);
    u8 result[] = {
        0x1d, 0x90, 0xce, 0x68, 0x45, 0x0b, 0xba, 0xd6, 0xbe, 0x1c, 0xb2, 0x3a, 0xea, 0x7f, 0xac, 0x4b, 0x68, 0x08, 0xa4, 0x77, 0x81, 0x2a, 0xad, 0x5d, 0x05, 0xe2, 0x15, 0xe8, 0xf4, 0xcb, 0x06, 0xaf
    };
    auto mac = hmac.process("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv);
    EXPECT(memcmp(result, mac.data(), hmac.digest_size()) == 0);
}

TEST_CASE(test_hmac_sha256_data_is_bigger_size_as_block)
{
    auto key = "Well Hello Friends"_b;
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::SHA256, key);
    u8 result[] = {
        0x9b, 0xa3, 0x9e, 0xf3, 0xb4, 0x30, 0x5f, 0x6f, 0x67, 0xd0, 0xa8, 0xb0, 0xf0, 0xcb, 0x12, 0xf5, 0x85, 0xe2, 0x19, 0xba, 0x0c, 0x8b, 0xe5, 0x43, 0xf0, 0x93, 0x39, 0xa8, 0xa3, 0x07, 0xf1, 0x95
    };
    auto mac = hmac.process("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv);
    EXPECT(memcmp(result, mac.data(), hmac.digest_size()) == 0);
}
