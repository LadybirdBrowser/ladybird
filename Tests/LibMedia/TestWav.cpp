/*
 * Copyright (c) 2024, Lee Hanken <github-12-2017-ds8@leehanken.uk>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibMedia/Audio/Loader.h>
#include <LibTest/TestCase.h>

static void run_test(StringView file_name, int const num_samples, int const channels, u32 const rate)
{
    constexpr auto format = "wav";
    constexpr int bits = 32;

    ByteString in_path = ByteString::formatted("WAV/{}", file_name);

    auto loader = TRY_OR_FAIL(Audio::Loader::create(in_path));

    EXPECT_EQ(loader->format_name(), format);
    EXPECT_EQ(loader->sample_rate(), rate);
    EXPECT_EQ(loader->num_channels(), channels);
    EXPECT_EQ(loader->bits_per_sample(), bits);
    EXPECT_EQ(loader->total_samples(), num_samples);
}

// 5 seconds, 16-bit audio samples

TEST_CASE(mono_8khz)
{
    run_test("tone_8000_mono.wav"sv, 40000, 1, 8000);
}

TEST_CASE(stereo_8khz)
{
    run_test("tone_8000_stereo.wav"sv, 40000, 2, 8000);
}

TEST_CASE(mono_11khz)
{
    run_test("tone_11025_mono.wav"sv, 55125, 1, 11025);
}

TEST_CASE(stereo_11khz)
{
    run_test("tone_11025_stereo.wav"sv, 55125, 2, 11025);
}

TEST_CASE(mono_16khz)
{
    run_test("tone_16000_mono.wav"sv, 80000, 1, 16000);
}

TEST_CASE(stereo_16khz)
{
    run_test("tone_16000_stereo.wav"sv, 80000, 2, 16000);
}

TEST_CASE(mono_22khz)
{
    run_test("tone_22050_mono.wav"sv, 110250, 1, 22050);
}

TEST_CASE(stereo_22khz)
{
    run_test("tone_22050_stereo.wav"sv, 110250, 2, 22050);
}

TEST_CASE(mono_44khz)
{
    run_test("tone_44100_mono.wav"sv, 220500, 1, 44100);
}

TEST_CASE(stereo_44khz)
{
    run_test("tone_44100_stereo.wav"sv, 220500, 2, 44100);
}
