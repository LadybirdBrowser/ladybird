/*
 * Copyright (c) 2021, Peter Bocan  <me@pbocan.net>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibCrypto/PK/RSA.h>
#include <LibTest/TestCase.h>

TEST_CASE(test_RSA_rejects_invalid_public_key)
{
    Array<u8, 1> message { 42 };
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, message);

    Array<u8, 128> modulus;
    modulus.fill(0xff);
    Array<u8, 128> encoded_message;
    encoded_message.fill(0xff);
    encoded_message[0] = 0;
    encoded_message[1] = 1;
    constexpr Array<u8, 19> digest_info_prefix {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
    };
    auto separator = encoded_message.size() - digest_info_prefix.size() - digest.bytes().size() - 1;
    encoded_message[separator] = 0;
    __builtin_memcpy(encoded_message.data() + separator + 1, digest_info_prefix.data(), digest_info_prefix.size());
    __builtin_memcpy(encoded_message.data() + separator + 1 + digest_info_prefix.size(), digest.bytes().data(), digest.bytes().size());

    u8 const exponent_one[] { 1 };
    EXPECT(!TRY_OR_FAIL(Crypto::PK::rsa_pkcs1_verify(modulus, exponent_one, Crypto::Hash::HashKind::SHA256, message, encoded_message)));
}
