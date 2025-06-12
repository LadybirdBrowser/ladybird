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
    auto rsa = Crypto::PK::RSA_OAEP_EME(Crypto::Hash::HashKind::SHA1, keypair);
    rsa.set_label("LABEL"sv.bytes());

    auto enc = TRY_OR_FAIL(rsa.encrypt(msg));
    auto dec = TRY_OR_FAIL(rsa.decrypt(enc));
    EXPECT_EQ(msg, dec);
}
