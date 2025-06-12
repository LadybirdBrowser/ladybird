/*
 * Copyright (c) 2020, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/NumberFormat.h>

/*
 * These tests are mostly meant as a rough sanity-check, to see whether
 * human_readable_size() crashes or does something very silly. That, however,
 * is a fuzzy human term, so these tests have to hard-code the exact expected
 * strings.
 *
 * Please feel free to tweak human_readable_size()'s behavior, and update the
 * "expected" strings below.
 */

TEST_CASE(golden_path)
{
    EXPECT_EQ(human_readable_size(0), "0 B");
    EXPECT_EQ(human_readable_size(123), "123 B");
    EXPECT_EQ(human_readable_size(123 * KiB), "123.0 KiB");
    EXPECT_EQ(human_readable_size(123 * MiB), "123.0 MiB");
    EXPECT_EQ(human_readable_size(2 * GiB), "2.0 GiB");
}

TEST_CASE(border_B_KiB)
{
    EXPECT_EQ(human_readable_size(1000), "1000 B");
    EXPECT_EQ(human_readable_size(1023), "1023 B");
    // KiB = 1024
    EXPECT_EQ(human_readable_size(1024), "1.0 KiB");
    EXPECT_EQ(human_readable_size(1025), "1.0 KiB");
}

TEST_CASE(fraction_KiB)
{
    EXPECT_EQ(human_readable_size(1050), "1.0 KiB");
    EXPECT_EQ(human_readable_size(1075), "1.0 KiB");
    // 1024 * 1.05 = 1075.2
    EXPECT_EQ(human_readable_size(1076), "1.0 KiB");

    EXPECT_EQ(human_readable_size(1100), "1.0 KiB");

    EXPECT_EQ(human_readable_size(1126), "1.0 KiB");
    // 1024 * 1.1 = 1126.4
    EXPECT_EQ(human_readable_size(1127), "1.1 KiB");
    EXPECT_EQ(human_readable_size(1146), "1.1 KiB");
}

TEST_CASE(border_KiB_MiB)
{
    EXPECT_EQ(human_readable_size(1000 * KiB), "1000.0 KiB");
    EXPECT_EQ(human_readable_size(1024 * KiB - 1), "1023.9 KiB");
    // MiB
    EXPECT_EQ(human_readable_size(1024 * KiB), "1.0 MiB");
    EXPECT_EQ(human_readable_size(1024 * KiB + 1), "1.0 MiB");
}

TEST_CASE(fraction_MiB)
{
    EXPECT_EQ(human_readable_size(1069547), "1.0 MiB");
    EXPECT_EQ(human_readable_size(1101004), "1.0 MiB");
    // 1024 * 1024 * 1.05 = 1101004.8
    EXPECT_EQ(human_readable_size(1101005), "1.0 MiB");
    EXPECT_EQ(human_readable_size(1101006), "1.0 MiB");

    EXPECT_EQ(human_readable_size(1120000), "1.0 MiB");

    EXPECT_EQ(human_readable_size(1153433), "1.0 MiB");
    // 1024 * 1024 * 1.1 = 1153433.6
    EXPECT_EQ(human_readable_size(1153434), "1.1 MiB");
}

TEST_CASE(border_MiB_GiB)
{
    EXPECT_EQ(human_readable_size(1000 * MiB), "1000.0 MiB");
    EXPECT_EQ(human_readable_size(1024 * MiB - 1), "1023.9 MiB");
    EXPECT_EQ(human_readable_size(1024 * MiB), "1.0 GiB");
    EXPECT_EQ(human_readable_size(1024 * MiB + 1), "1.0 GiB");
}

TEST_CASE(fraction_GiB)
{
    EXPECT_EQ(human_readable_size(1095216660), "1.0 GiB");
    EXPECT_EQ(human_readable_size(1127428915), "1.0 GiB");
    // 1024 * 1024 * 1024 * 1.05 = 1127428915.2
    EXPECT_EQ(human_readable_size(1127428916), "1.0 GiB");
    EXPECT_EQ(human_readable_size(1127536289), "1.0 GiB");

    EXPECT_EQ(human_readable_size(1154272461), "1.0 GiB");

    EXPECT_EQ(human_readable_size(1181115968), "1.0 GiB");
    EXPECT_EQ(human_readable_size(1181115969), "1.0 GiB");
    EXPECT_EQ(human_readable_size(1181116000), "1.0 GiB");
    EXPECT_EQ(human_readable_size(1181116006), "1.0 GiB");
    // 1024 * 1024 * 1024 * 1.1 = 1181116006.4
    EXPECT_EQ(human_readable_size(1181116007), "1.1 GiB");
    EXPECT_EQ(human_readable_size(1202590842), "1.1 GiB");
}

TEST_CASE(extremes_4byte)
{
    EXPECT_EQ(human_readable_size(0x7fffffff), "1.9 GiB");
    EXPECT_EQ(human_readable_size(0x80000000), "2.0 GiB");
    EXPECT_EQ(human_readable_size(0xffffffff), "3.9 GiB");
}

TEST_CASE(extremes_8byte)
{
    if constexpr (sizeof(size_t) == 8) {
        warnln("(Running 8-byte-size_t test)");
        EXPECT_EQ(human_readable_size(0x100000000ULL), "4.0 GiB");
        EXPECT_EQ(human_readable_size(0x100000001ULL), "4.0 GiB");
        EXPECT_EQ(human_readable_size(0x800000000ULL), "32.0 GiB");
        EXPECT_EQ(human_readable_size(0x10000000000ULL), "1.0 TiB");
        EXPECT_EQ(human_readable_size(0x4000000000000ULL), "1.0 PiB");
        EXPECT_EQ(human_readable_size(0x7fffffffffffffffULL), "7.9 EiB");
        EXPECT_EQ(human_readable_size(0x8000000000000000ULL), "8.0 EiB");
        EXPECT_EQ(human_readable_size(0xffffffffffffffffULL), "15.9 EiB");
    } else {
        warnln("(Skipping 8-byte-size_t test on 32-bit platform)");
    }
}

TEST_CASE(base10_units)
{
    EXPECT_EQ(human_readable_size(999, AK::HumanReadableBasedOn::Base10), "999 B");
    EXPECT_EQ(human_readable_size(1024, AK::HumanReadableBasedOn::Base10), "1.0 KB");
    EXPECT_EQ(human_readable_size(1100, AK::HumanReadableBasedOn::Base10), "1.1 KB");
    EXPECT_EQ(human_readable_size(1000000, AK::HumanReadableBasedOn::Base10), "1.0 MB");
}
