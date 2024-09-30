/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/Loader.h>
#include <LibTest/TestCase.h>

static void run_test(StringView file_name, int const num_samples, int const channels, u32 const rate)
{
    constexpr auto format = "ogg";
    constexpr int bits = 32;

    ByteString in_path = ByteString::formatted("vorbis/{}", file_name);

    auto loader = TRY_OR_FAIL(Audio::Loader::create(in_path));

    EXPECT_EQ(loader->format_name(), format);
    EXPECT_EQ(loader->sample_rate(), rate);
    EXPECT_EQ(loader->num_channels(), channels);
    EXPECT_EQ(loader->bits_per_sample(), bits);
    EXPECT_EQ(loader->total_samples(), num_samples);
}

TEST_CASE(44_1Khz_stereo)
{
    run_test("44_1Khz_stereo.ogg"sv, 352800, 2, 44100);
}
