/*
 * Copyright (c) 2024, stelar7  <dudedbz@gmail.com>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCrypto/PK/RSA.h>
#include <LibTest/TestCase.h>

// https://www.inf.pucrs.br/~calazans/graduate/TPVLSI_I/RSA-oaep_spec.pdf
TEST_CASE(test_oaep)
{
    auto msg = "WellHelloFriendsWellHelloFriendsWellHelloFriendsWellHelloFriends"sv.bytes();

    auto keypair = TRY_OR_FAIL(Crypto::PK::RSA::generate_key_pair(1024));
    auto label = "LABEL"sv.bytes();

    auto enc = TRY_OR_FAIL(Crypto::PK::rsa_oaep_encrypt(keypair.public_key, Crypto::Hash::HashKind::SHA1, label, msg));
    auto dec = TRY_OR_FAIL(Crypto::PK::rsa_oaep_decrypt(keypair.private_key, Crypto::Hash::HashKind::SHA1, label, enc));
    EXPECT_EQ(msg, dec);
}
