/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/PK/MLDSA.h>
#include <LibTest/TestCase.h>

TEST_CASE(TEST_SIGN_AND_VERIFY)
{
    u8 message_bytes[12] = { 0x48, 0X65, 0X6C, 0X6C, 0X6F, 0X20, 0X57, 0X6F, 0X72, 0X6C, 0X64, 0X21 }; // "Hello World!"

    ReadonlyBytes message(message_bytes, 12);

    auto [_, private_key] = TRY_OR_FAIL(Crypto::PK::MLDSA::generate_key_pair(Crypto::PK::MLDSA65));

    Crypto::PK::MLDSA lattice { Crypto::PK::MLDSA65, private_key, {} };
    Crypto::PK::MLDSA lattice_with_context { Crypto::PK::MLDSA65, private_key, TRY_OR_FAIL(ByteBuffer::copy(message_bytes)) };

    auto const signed_message = TRY_OR_FAIL(lattice.sign(message));
    EXPECT(signed_message.size() == 3309);
    EXPECT(TRY_OR_FAIL(lattice.verify(message, signed_message)));

    auto const signed_message_with_context = TRY_OR_FAIL(lattice_with_context.sign(message));
    EXPECT(signed_message_with_context.size() == 3309);
    EXPECT_NE(signed_message, signed_message_with_context);
    EXPECT(TRY_OR_FAIL(lattice_with_context.verify(message, signed_message_with_context)));
}
