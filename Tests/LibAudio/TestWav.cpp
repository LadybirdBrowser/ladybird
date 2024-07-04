/*
 * Copyright (c) 2024, Lee Hanken <github-12-2017-ds8@leehanken.uk>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <LibAudio/WavLoader.h>
#include <LibAudio/WavWriter.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibFileSystem/TempFile.h>
#include <LibTest/TestCase.h>

static void compare_files(StringView const& in_path, StringView const& out_path)
{
    Array<u8, 4096> buffer1;
    Array<u8, 4096> buffer2;

    auto original_file = MUST(Core::File::open(in_path, Core::File::OpenMode::Read));
    auto copied_file = MUST(Core::File::open(out_path, Core::File::OpenMode::Read));

    while (!original_file->is_eof() && !copied_file->is_eof()) {
        auto original_bytes = TRY_OR_FAIL(original_file->read_some(buffer1));
        auto copied_bytes = TRY_OR_FAIL(copied_file->read_some(buffer2));

        EXPECT_EQ(original_bytes, copied_bytes);
    }
}

static void run_test(StringView file_name, int const num_samples, int const channels, u32 const rate)
{

    constexpr auto format = "RIFF WAVE (.wav)";
    constexpr int bits = 16;

    auto out_file = TRY_OR_FAIL(FileSystem::TempFile::create_temp_file());
    auto out_path = out_file->path();

    ByteString in_path = ByteString::formatted("WAV/{}", file_name);

    auto loader = TRY_OR_FAIL(Audio::Loader::create(in_path));

    EXPECT_EQ(loader->format_name(), format);
    EXPECT_EQ(loader->sample_rate(), rate);
    EXPECT_EQ(loader->num_channels(), channels);
    EXPECT_EQ(loader->bits_per_sample(), bits);
    EXPECT_EQ(loader->total_samples(), num_samples);

    auto writer = TRY_OR_FAIL(Audio::WavWriter::create_from_file(out_path, rate, channels));

    int samples_read = 0;
    int size = 0;

    do {
        auto samples = TRY_OR_FAIL(loader->get_more_samples());
        TRY_OR_FAIL(writer->write_samples(samples.span()));

        size = samples.size();
        samples_read += size;

    } while (size);

    TRY_OR_FAIL(writer->finalize());

    EXPECT_EQ(samples_read, num_samples);

    compare_files(in_path, out_path);
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
