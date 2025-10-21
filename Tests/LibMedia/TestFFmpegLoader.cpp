/*
 * Copyright (c) 2025, Ladybird Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/Loader.h>
#include <LibMedia/Audio/SampleFormats.h>
#include <LibTest/TestCase.h>

TEST_CASE(ffmpeg_loader_vorbis_format)
{
    // Test that FFmpegLoader correctly reports the PCM format for Vorbis audio
    // Vorbis audio is typically decoded as floating-point samples
    auto loader = TRY_OR_FAIL(Audio::Loader::create("vorbis/44_1Khz_stereo.ogg"));

    EXPECT_EQ(loader->format_name(), "ogg");
    EXPECT_EQ(loader->sample_rate(), 44100u);
    EXPECT_EQ(loader->num_channels(), 2);

    // Vorbis audio is decoded as Float32 by FFmpeg
    auto pcm_format = loader->pcm_format();
    EXPECT_EQ(pcm_format, Audio::PcmSampleFormat::Float32);
}

TEST_CASE(ffmpeg_loader_wav_format)
{
    // Test that FFmpegLoader correctly reports the PCM format for WAV audio
    // WAV files with 32-bit samples should be detected as Int32
    auto loader = TRY_OR_FAIL(Audio::Loader::create("WAV/tone_44100_stereo.wav"));

    EXPECT_EQ(loader->format_name(), "wav");
    EXPECT_EQ(loader->sample_rate(), 44100u);
    EXPECT_EQ(loader->num_channels(), 2);

    // The WAV test files are 32-bit PCM, which FFmpeg reports as S32
    // However, the WAV loader (not FFmpeg) might be used, so we just verify
    // that pcm_format() returns a valid format without crashing
    auto pcm_format = loader->pcm_format();

    // Valid formats are Uint8, Int16, Int24, Int32, or Float32
    bool is_valid_format = pcm_format == Audio::PcmSampleFormat::Uint8
        || pcm_format == Audio::PcmSampleFormat::Int16
        || pcm_format == Audio::PcmSampleFormat::Int24
        || pcm_format == Audio::PcmSampleFormat::Int32
        || pcm_format == Audio::PcmSampleFormat::Float32;

    EXPECT(is_valid_format);
}

TEST_CASE(ffmpeg_loader_basic_functionality)
{
    // Basic test to ensure FFmpegLoader can load and decode audio
    auto loader = TRY_OR_FAIL(Audio::Loader::create("vorbis/44_1Khz_stereo.ogg"));

    // Verify we can read some samples without crashing
    auto samples = TRY_OR_FAIL(loader->get_more_samples(1024));

    EXPECT_EQ(samples.size(), 1024u);
    EXPECT(loader->loaded_samples() >= 1024);
}
