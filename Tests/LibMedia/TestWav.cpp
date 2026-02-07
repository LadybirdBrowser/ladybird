/*
 * Copyright (c) 2024, Lee Hanken <github-12-2017-ds8@leehanken.uk>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Tests/LibMedia/TestMediaCommon.h>

// 5 seconds, 16-bit audio samples

TEST_CASE(mono_8khz)
{
    decode_audio("WAV/tone_8000_mono.wav"sv, 8000, 1, 40000);
}

TEST_CASE(stereo_8khz)
{
    decode_audio("WAV/tone_8000_stereo.wav"sv, 8000, 2, 40000);
}

TEST_CASE(mono_11khz)
{
    decode_audio("WAV/tone_11025_mono.wav"sv, 11025, 1, 55125);
}

TEST_CASE(stereo_11khz)
{
    decode_audio("WAV/tone_11025_stereo.wav"sv, 11025, 2, 55125);
}

TEST_CASE(mono_16khz)
{
    decode_audio("WAV/tone_16000_mono.wav"sv, 16000, 1, 80000);
}

TEST_CASE(stereo_16khz)
{
    decode_audio("WAV/tone_16000_stereo.wav"sv, 16000, 2, 80000);
}

TEST_CASE(mono_22khz)
{
    decode_audio("WAV/tone_22050_mono.wav"sv, 22050, 1, 110250);
}

TEST_CASE(stereo_22khz)
{
    decode_audio("WAV/tone_22050_stereo.wav"sv, 22050, 2, 110250);
}

TEST_CASE(mono_44khz)
{
    decode_audio("WAV/tone_44100_mono.wav"sv, 44100, 1, 220500);
}

TEST_CASE(stereo_44khz)
{
    decode_audio("WAV/tone_44100_stereo.wav"sv, 44100, 2, 220500);
}

TEST_CASE(stereo_8khz_24bit)
{
    decode_audio("WAV/voices_8000_stereo_int24.wav"sv, 8000, 2, 23493);
}

TEST_CASE(underspecified_5_1_44khz)
{
    decode_audio("WAV/tone_44100_5_1_underspecified.wav"sv, 44100, 6, 44100, Audio::ChannelMap::surround_5_1());
}
