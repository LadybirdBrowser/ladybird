/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Hash/PBKDF2.h>
#include <LibTest/TestCase.h>

// https://www.rfc-editor.org/rfc/rfc6070#section-2
TEST_CASE(test_vector_1_sha1)
{
    Array<u8, 8> const password {
        0x70, 0x61, 0x73, 0x73, 0x77, 0x6f, 0x72, 0x64
    };
    Array<u8, 4> const salt {
        0x73, 0x61, 0x6c, 0x74
    };
    Array<u8, 20> const expected {
        0x0c, 0x60, 0xc8, 0x0f, 0x96, 0x1f, 0x0e, 0x71,
        0xf3, 0xa9, 0xb5, 0x24, 0xaf, 0x60, 0x12, 0x06,
        0x2f, 0xe0, 0x37, 0xa6
    };
    u32 iterations = 1;
    u32 derived_key_length_bytes = 20;

    Crypto::Hash::PBKDF2 pbkdf2(Crypto::Hash::HashKind::SHA1);
    auto result = MUST(pbkdf2.derive_key(password, salt, iterations, derived_key_length_bytes));

    EXPECT_EQ(result, expected.span());
}

TEST_CASE(test_vector_1_sha256)
{
    Array<u8, 8> const password {
        0x70, 0x61, 0x73, 0x73, 0x77, 0x6f, 0x72, 0x64
    };
    Array<u8, 4> const salt {
        0x73, 0x61, 0x6c, 0x74
    };
    Array<u8, 20> const expected {
        0x12, 0x0f, 0xb6, 0xcf, 0xfc, 0xf8, 0xb3, 0x2c,
        0x43, 0xe7, 0x22, 0x52, 0x56, 0xc4, 0xf8, 0x37,
        0xa8, 0x65, 0x48, 0xc9
    };
    u32 iterations = 1;
    u32 derived_key_length_bytes = 20;

    Crypto::Hash::PBKDF2 pbkdf2(Crypto::Hash::HashKind::SHA256);
    auto result = MUST(pbkdf2.derive_key(password, salt, iterations, derived_key_length_bytes));

    EXPECT_EQ(result, expected.span());
}
