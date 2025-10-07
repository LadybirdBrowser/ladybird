/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/MappedFile.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/MutexedDemuxer.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibTest/TestCase.h>

static void run_test(StringView file_name, int const num_samples, int const channels, u32 const rate)
{
    Core::EventLoop loop;

    ByteString in_path = ByteString::formatted("{}", file_name);

    auto mapped_file = TRY_OR_FAIL(Core::MappedFile::map(in_path));
    auto demuxer = TRY_OR_FAIL(Media::FFmpeg::FFmpegDemuxer::from_data(mapped_file->bytes()));
    auto mutexed_demuxer = make_ref_counted<Media::MutexedDemuxer>(demuxer);
    auto track = TRY_OR_FAIL(demuxer->get_preferred_track_for_type(Media::TrackType::Audio));
    VERIFY(track.has_value());
    auto provider = TRY_OR_FAIL(Media::AudioDataProvider::try_create(mutexed_demuxer, track.release_value()));

    auto reached_end = false;
    provider->set_error_handler([&](Media::DecoderError&& error) {
        if (error.category() == Media::DecoderErrorCategory::EndOfStream) {
            reached_end = true;
            return;
        }
        FAIL("An error occurred while decoding.");
    });

    auto time_limit = AK::Duration::from_seconds(1);
    auto start_time = MonotonicTime::now_coarse();

    i64 sample_count = 0;

    while (true) {
        auto block = provider->retrieve_block();
        if (block.is_empty()) {
            if (reached_end)
                break;
        } else {
            EXPECT_EQ(block.sample_rate(), rate);
            EXPECT_EQ(block.channel_count(), channels);

            sample_count += block.sample_count();
        }

        if (MonotonicTime::now_coarse() - start_time >= time_limit) {
            FAIL("Decoding timed out.");
            return;
        }

        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    VERIFY(reached_end);
    EXPECT_EQ(sample_count, num_samples);
}

TEST_CASE(44_1Khz_stereo)
{
    run_test("vorbis/44_1Khz_stereo.ogg"sv, 352896, 2, 44100);
}
