/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCrypto/PK/RSA.h>
#include <LibTest/TestCase.h>

TEST_CASE(test_pss)
{
    auto msg = "WellHelloFriendsWellHelloFriendsWellHelloFriendsWellHelloFriends"sv.bytes();

    auto keypair = TRY_OR_FAIL(Crypto::PK::RSA::generate_key_pair(1024));
    auto rsa = Crypto::PK::RSA_PSS_EMSA(Crypto::Hash::HashKind::SHA1, keypair);
    rsa.set_salt_length(48);

    auto sig = TRY_OR_FAIL(rsa.sign(msg));
    auto ok = TRY_OR_FAIL(rsa.verify(msg, sig));
    EXPECT(ok);
}

TEST_CASE(test_pss_tampered_message)
{
    auto msg = "WellHelloFriendsWellHelloFriendsWellHelloFriendsWellHelloFriends"sv.bytes();
    auto msg_tampered = "WellHell0FriendsWellHelloFriendsWellHelloFriendsWellHelloFriends"sv.bytes();

    auto keypair = TRY_OR_FAIL(Crypto::PK::RSA::generate_key_pair(1024));
    auto rsa = Crypto::PK::RSA_PSS_EMSA(Crypto::Hash::HashKind::SHA1, keypair);
    rsa.set_salt_length(48);

    auto sig = TRY_OR_FAIL(rsa.sign(msg));
    auto ok = TRY_OR_FAIL(rsa.verify(msg_tampered, sig));
    EXPECT(!ok);
}

TEST_CASE(test_pss_tampered_signature)
{
    auto msg = "WellHelloFriendsWellHelloFriendsWellHelloFriendsWellHelloFriends"sv.bytes();

    auto keypair = TRY_OR_FAIL(Crypto::PK::RSA::generate_key_pair(1024));
    auto rsa = Crypto::PK::RSA_PSS_EMSA(Crypto::Hash::HashKind::SHA1, keypair);
    rsa.set_salt_length(48);

    auto sig = TRY_OR_FAIL(rsa.sign(msg));

    // Tamper with the signature
    sig[8]++;

    auto ok = TRY_OR_FAIL(rsa.verify(msg, sig));
    EXPECT(!ok);
}
