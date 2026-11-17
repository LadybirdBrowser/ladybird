/*
 * Copyright (c) 2021, Peter Bocan  <me@pbocan.net>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/Hash.h>
#include <LibTest/TestCase.h>
#include <cstring>

TEST_CASE(test_SHA1_hash_successive_updates)
{
    u8 result[] {
        0xd6, 0x6e, 0xce, 0xd1, 0xf4, 0x08, 0xc6, 0xd8, 0x35, 0xab, 0xf0, 0xc9, 0x05, 0x26, 0xa4, 0xb2, 0xb8, 0xa3, 0x7c, 0xd3
    };
    Crypto::Hash::Hasher hasher { Crypto::Hash::HashKind::SHA1 };
    hasher.update("aaaaaaaaaaaaaaa"sv);
    hasher.update("aaaaaaaaaaaaaaa"sv);
    hasher.update("aaaaaaaaaaaaaaa"sv);
    hasher.update("aaaaaaaaaaaaaaa"sv);
    hasher.update("aaaaaaaaaaaaaaa"sv);
    hasher.update("aaaaaaaaaaaaaaa"sv);
    hasher.update("aaaaaaaaaaaaaaa"sv);
    hasher.update("aaaaaaaaaaaaaaa"sv);
    hasher.update("aaaaaaaaaaaaaaa"sv);
    hasher.update("aaaaaaaaaaaaaaa"sv);
    hasher.update("aaaaaaaaaaaaaaa"sv);
    hasher.update("aaaaaaaaaaaaaaa"sv);
    hasher.update("aaaaaaaaa"sv);
    auto digest = hasher.finish();
    EXPECT_EQ(digest.bytes(), ReadonlyBytes { result });
}

TEST_CASE(test_SHA256_hash_string)
{
    u8 result[] {
        0x9a, 0xcd, 0x50, 0xf9, 0xa2, 0xaf, 0x37, 0xe4, 0x71, 0xf7, 0x61, 0xc3, 0xfe, 0x7b, 0x8d, 0xea, 0x56, 0x17, 0xe5, 0x1d, 0xac, 0x80, 0x2f, 0xe6, 0xc1, 0x77, 0xb7, 0x4a, 0xbf, 0x0a, 0xbb, 0x5a
    };
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, "Well hello friends"sv.bytes());
    EXPECT_EQ(digest.bytes(), ReadonlyBytes { result });
}

TEST_CASE(test_SHA256_hash_split_into_blocks)
{
    u8 result[] {
        0x1b, 0x90, 0x74, 0xf9, 0x9b, 0xf3, 0x56, 0x20, 0xd3, 0xf6, 0xbf, 0x7d, 0x72, 0x53, 0xb3, 0x3c, 0xd1, 0x1b, 0x7d, 0xd8, 0xa9, 0xa0, 0x4e, 0x55, 0xf5, 0x72, 0x43, 0x94, 0xaf, 0x27, 0x6c, 0x5c
    };
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, "0123456789012345678901234567890123456789012345678901234567890123abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcd"sv.bytes());
    EXPECT_EQ(digest.bytes(), ReadonlyBytes { result });
}

TEST_CASE(test_SHA3_256_hash_split_into_blocks)
{
    u8 result[] {
        0x0c, 0x4e, 0x02, 0xfb, 0x73, 0x31, 0xe9, 0xe7, 0x25, 0x8c, 0xcf, 0xc6, 0x25, 0x03, 0xe9, 0x2c, 0xa5, 0x7f, 0xaa, 0x91, 0x4a, 0x21, 0xde, 0x00, 0x03, 0x72, 0x9a, 0xa0, 0xaa, 0x0d, 0x9e, 0x76
    };
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA3_256, "0123456789012345678901234567890123456789012345678901234567890123abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcd"sv.bytes());
    EXPECT_EQ(digest.bytes(), ReadonlyBytes { result });
}
