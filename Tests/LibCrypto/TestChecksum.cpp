/*
 * Copyright (c) 2021, Peter Bocan  <me@pbocan.net>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/Checksum/cksum.h>
#include <LibTest/TestCase.h>

TEST_CASE(test_cksum)
{
    auto do_test = [](ReadonlyBytes input, u32 expected_result) {
        auto digest = Crypto::Checksum::cksum(input).digest();
        EXPECT_EQ(digest, expected_result);
    };

    do_test(""sv.bytes(), 0xFFFFFFFF);
    do_test("The quick brown fox jumps over the lazy dog"sv.bytes(), 0x7BAB9CE8);
    do_test("various CRC algorithms input data"sv.bytes(), 0xEFB5CA4F);
}

TEST_CASE(test_cksum_atomic_digest)
{
    auto compare = [](u32 digest, u32 expected_result) {
        EXPECT_EQ(digest, expected_result);
    };

    Crypto::Checksum::cksum cksum;

    cksum.update("Well"sv.bytes());
    cksum.update(" hello "sv.bytes());
    cksum.digest();
    cksum.update("friends"sv.bytes());
    auto digest = cksum.digest();

    compare(digest, 0x2D65C7E0);
}
