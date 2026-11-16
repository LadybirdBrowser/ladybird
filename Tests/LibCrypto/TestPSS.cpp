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

    auto sig = TRY_OR_FAIL(Crypto::PK::rsa_pss_sign(keypair.private_key, Crypto::Hash::HashKind::SHA1, 48, msg));
    auto ok = TRY_OR_FAIL(Crypto::PK::rsa_pss_verify(keypair.public_key, Crypto::Hash::HashKind::SHA1, 48, msg, sig));
    EXPECT(ok);
}

TEST_CASE(test_pss_tampered_message)
{
    auto msg = "WellHelloFriendsWellHelloFriendsWellHelloFriendsWellHelloFriends"sv.bytes();
    auto msg_tampered = "WellHell0FriendsWellHelloFriendsWellHelloFriendsWellHelloFriends"sv.bytes();

    auto keypair = TRY_OR_FAIL(Crypto::PK::RSA::generate_key_pair(1024));

    auto sig = TRY_OR_FAIL(Crypto::PK::rsa_pss_sign(keypair.private_key, Crypto::Hash::HashKind::SHA1, 48, msg));
    auto ok = TRY_OR_FAIL(Crypto::PK::rsa_pss_verify(keypair.public_key, Crypto::Hash::HashKind::SHA1, 48, msg_tampered, sig));
    EXPECT(!ok);
}

TEST_CASE(test_pss_tampered_signature)
{
    auto msg = "WellHelloFriendsWellHelloFriendsWellHelloFriendsWellHelloFriends"sv.bytes();

    auto keypair = TRY_OR_FAIL(Crypto::PK::RSA::generate_key_pair(1024));

    auto sig = TRY_OR_FAIL(Crypto::PK::rsa_pss_sign(keypair.private_key, Crypto::Hash::HashKind::SHA1, 48, msg));

    // Tamper with the signature
    sig[8]++;

    auto ok = TRY_OR_FAIL(Crypto::PK::rsa_pss_verify(keypair.public_key, Crypto::Hash::HashKind::SHA1, 48, msg, sig));
    EXPECT(!ok);
}
