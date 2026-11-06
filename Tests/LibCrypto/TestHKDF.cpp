/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Ben Wiederhake
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/HKDF.h>
#include <LibTest/TestCase.h>

TEST_CASE(test_error_extreme_output_key_length)
{
    Crypto::Hash::HKDF hkdf(Crypto::Hash::HashKind::SHA1);
    auto result = hkdf.derive_key(Optional<ReadonlyBytes>(), ReadonlyBytes(), ReadonlyBytes(), 999999);
    EXPECT(result.is_error());
}

// https://www.rfc-editor.org/rfc/rfc5869#appendix-A.1
TEST_CASE(test_vector_A_1)
{
    // IKM  = 0x0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b (22 octets)
    u8 const ikm[] {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b
    };
    static_assert(sizeof(ikm) == 22);
    // salt = 0x000102030405060708090a0b0c (13 octets)
    u8 const salt[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c };
    static_assert(sizeof(salt) == 13);
    // info = 0xf0f1f2f3f4f5f6f7f8f9 (10 octets)
    u8 const info[] = { 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9 };
    static_assert(sizeof(info) == 10);
    // L = 42
    size_t const output_key_length = 42;

    Crypto::Hash::HKDF hkdf(Crypto::Hash::HashKind::SHA256);
    auto result = TRY_OR_FAIL(hkdf.derive_key(Optional<ReadonlyBytes>(salt), ikm, info, output_key_length));
    // Intermediate value 'PRK' isn't checked explicitly. However, any bit error would have
    // an avalanche effect on the output, so if the output is correct then PRK can be presumed correct, too.
    u8 const expected_output_key[] = {
        0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a, 0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36, 0x2f, 0x2a,
        0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a, 0x5a, 0x4c, 0x5d, 0xb0, 0x2d, 0x56, 0xec, 0xc4, 0xc5, 0xbf,
        0x34, 0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18, 0x58, 0x65
    };
    static_assert(sizeof(expected_output_key) == output_key_length);
    EXPECT_EQ(result.bytes(), ReadonlyBytes(expected_output_key));
}

// https://www.rfc-editor.org/rfc/rfc5869#appendix-A.3
TEST_CASE(test_vector_A_3)
{
    u8 const ikm[] {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b
    };
    static_assert(sizeof(ikm) == 22);
    size_t const output_key_length = 42;

    Crypto::Hash::HKDF hkdf(Crypto::Hash::HashKind::SHA256);

    // Note: This creates a salt of zero bytes.
    auto result = TRY_OR_FAIL(hkdf.derive_key(Optional<ReadonlyBytes>(ReadonlyBytes()), ikm, ReadonlyBytes(), output_key_length));
    // Intermediate value 'PRK' isn't checked explicitly. However, any bit error would have
    // an avalanche effect on the output, so if the output is correct then PRK can be presumed correct, too.
    u8 const expected_output_key[] = {
        0x8d, 0xa4, 0xe7, 0x75, 0xa5, 0x63, 0xc1, 0x8f, 0x71, 0x5f, 0x80, 0x2a, 0x06, 0x3c, 0x5a, 0x31,
        0xb8, 0xa1, 0x1f, 0x5c, 0x5e, 0xe1, 0x87, 0x9e, 0xc3, 0x45, 0x4e, 0x5f, 0x3c, 0x73, 0x8d, 0x2d,
        0x9d, 0x20, 0x13, 0x95, 0xfa, 0xa4, 0xb6, 0x1a, 0x96, 0xc8
    };
    static_assert(sizeof(expected_output_key) == output_key_length);
    EXPECT_EQ(result.bytes(), ReadonlyBytes(expected_output_key));
}
