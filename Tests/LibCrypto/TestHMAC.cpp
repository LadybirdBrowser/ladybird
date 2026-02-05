/*
 * Copyright (c) 2021, the Ladybird developers.
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

TEST_CASE(test_hmac_md5_name)
{
    auto key = "Well Hello Friends"_b;
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::MD5, key);
    EXPECT_EQ(hmac.class_name(), "HMAC-MD5"sv);
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

TEST_CASE(test_hmac_md5_process_reuse)
{
    auto key = "Well Hello Friends"_b;
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::MD5, key);

    auto mac_0 = hmac.process("Some bogus data"sv);
    auto mac_1 = hmac.process("Some bogus data"sv);

    EXPECT(memcmp(mac_0.data(), mac_1.data(), hmac.digest_size()) == 0);
}

TEST_CASE(test_hmac_sha1_name)
{
    auto key = "Well Hello Friends"_b;
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::SHA1, key);
    EXPECT_EQ(hmac.class_name(), "HMAC-SHA1"sv);
}

TEST_CASE(test_hmac_sha1_process)
{
    u8 key[] { 0xc8, 0x52, 0xe5, 0x4a, 0x2c, 0x03, 0x2b, 0xc9, 0x63, 0xd3, 0xc2, 0x79, 0x0f, 0x76, 0x43, 0xef, 0x36, 0xc3, 0x7a, 0xca };
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::SHA1, ReadonlyBytes { key, sizeof(key) });
    u8 result[] {
        0x2c, 0x57, 0x32, 0x61, 0x3b, 0xa7, 0x84, 0x87, 0x0e, 0x4f, 0x42, 0x07, 0x2f, 0xf0, 0xe7, 0x41, 0xd7, 0x15, 0xf4, 0x56
    };
    u8 value[] {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x03, 0x03, 0x00, 0x10, 0x14, 0x00, 0x00, 0x0c, 0xa1, 0x91, 0x1a, 0x20, 0x59, 0xb5, 0x45, 0xa9, 0xb4, 0xad, 0x75, 0x3e
    };
    auto mac = hmac.process(value, 29);
    EXPECT(memcmp(result, mac.data(), hmac.digest_size()) == 0);
}

TEST_CASE(test_hmac_sha1_process_reuse)
{
    u8 key[] { 0xc8, 0x52, 0xe5, 0x4a, 0x2c, 0x03, 0x2b, 0xc9, 0x63, 0xd3, 0xc2, 0x79, 0x0f, 0x76, 0x43, 0xef, 0x36, 0xc3, 0x7a, 0xca };
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::SHA1, ReadonlyBytes { key, sizeof(key) });
    u8 result[] {
        0x2c, 0x57, 0x32, 0x61, 0x3b, 0xa7, 0x84, 0x87, 0x0e, 0x4f, 0x42, 0x07, 0x2f, 0xf0, 0xe7, 0x41, 0xd7, 0x15, 0xf4, 0x56
    };
    u8 value[] {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x03, 0x03, 0x00, 0x10, 0x14, 0x00, 0x00, 0x0c, 0xa1, 0x91, 0x1a, 0x20, 0x59, 0xb5, 0x45, 0xa9, 0xb4, 0xad, 0x75, 0x3e
    };
    hmac.update(value, 8);
    hmac.update(value + 8, 5);
    hmac.update(value + 13, 16);
    auto mac = hmac.digest();
    EXPECT(memcmp(result, mac.data(), hmac.digest_size()) == 0);
}

TEST_CASE(test_hmac_sha256_name)
{
    auto key = "Well Hello Friends"_b;
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::SHA256, key);
    EXPECT_EQ(hmac.class_name(), "HMAC-SHA256"sv);
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

TEST_CASE(test_hmac_sha512_name)
{
    auto key = "Well Hello Friends"_b;
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::SHA512, key);
    EXPECT_EQ(hmac.class_name(), "HMAC-SHA512");
}

TEST_CASE(test_hmac_sha512_process)
{
    auto key = "Well Hello Friends"_b;
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::SHA512, key);
    u8 result[] {
        0xeb, 0xa8, 0x34, 0x11, 0xfd, 0x5b, 0x46, 0x5b, 0xef, 0xbb, 0x67, 0x5e, 0x7d, 0xc2, 0x7c, 0x2c, 0x6b, 0xe1, 0xcf, 0xe6, 0xc7, 0xe4, 0x7d, 0xeb, 0xca, 0x97, 0xb7, 0x4c, 0xd3, 0x4d, 0x6f, 0x08, 0x9f, 0x0d, 0x3a, 0xf1, 0xcb, 0x00, 0x79, 0x78, 0x2f, 0x05, 0x8e, 0xeb, 0x94, 0x48, 0x0d, 0x50, 0x64, 0x3b, 0xca, 0x70, 0xe2, 0x69, 0x38, 0x4f, 0xe4, 0xb0, 0x49, 0x0f, 0xc5, 0x4c, 0x7a, 0xa7
    };
    auto mac = hmac.process("Some bogus data"sv);
    EXPECT(memcmp(result, mac.data(), hmac.digest_size()) == 0);
}

TEST_CASE(test_hmac_sha512_reuse)
{
    auto key = "Well Hello Friends"_b;
    Crypto::Authentication::HMAC hmac(Crypto::Hash::HashKind::SHA512, key);

    auto mac_0 = hmac.process("Some bogus data"sv);
    auto mac_1 = hmac.process("Some bogus data"sv);

    EXPECT(memcmp(mac_0.data(), mac_1.data(), hmac.digest_size()) == 0);
}
