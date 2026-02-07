/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/Containers/Matroska/Reader.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFrame.h>
#include <LibTest/TestCase.h>

template<typename T>
static inline void decode_video(StringView path, size_t expected_frame_count, T create_decoder)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto matroska_reader = MUST(Media::Matroska::Reader::from_stream(stream->create_cursor()));
    u64 video_track = 0;
    MUST(matroska_reader.for_each_track_of_type(Media::Matroska::TrackEntry::TrackType::Video, [&](Media::Matroska::TrackEntry const& track_entry) -> Media::DecoderErrorOr<IterationDecision> {
        video_track = track_entry.track_number();
        return IterationDecision::Break;
    }));
    VERIFY(video_track != 0);

    auto iterator = MUST(matroska_reader.create_sample_iterator(stream->create_cursor(), video_track));
    size_t frame_count = 0;
    NonnullOwnPtr<Media::VideoDecoder> decoder = create_decoder(iterator);

    auto last_timestamp = AK::Duration::min();

    while (frame_count <= expected_frame_count) {
        auto block_result = iterator.next_block();
        if (block_result.is_error() && block_result.error().category() == Media::DecoderErrorCategory::EndOfStream) {
            VERIFY(frame_count == expected_frame_count);
            return;
        }

        auto block = block_result.release_value();
        auto frames = MUST(iterator.get_frames(block));
        for (auto const& frame : frames) {
            MUST(decoder->receive_coded_data(block.timestamp(), block.duration().value_or(AK::Duration::zero()), frame));
            while (true) {
                auto frame_result = decoder->get_decoded_frame({});
                if (frame_result.is_error()) {
                    if (frame_result.error().category() == Media::DecoderErrorCategory::NeedsMoreInput)
                        break;
                    VERIFY_NOT_REACHED();
                }
                EXPECT(last_timestamp <= frame_result.value()->timestamp());
                last_timestamp = frame_result.value()->timestamp();
            }
            frame_count++;
        }
    }

    VERIFY_NOT_REACHED();
}

static inline void decode_audio(StringView path, u32 sample_rate, u8 channel_count, size_t expected_sample_count, Optional<Audio::ChannelMap> expected_channel_map = {})
{
    Core::EventLoop loop;

    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
    auto demuxer = MUST([&] -> Media::DecoderErrorOr<NonnullRefPtr<Media::Demuxer>> {
        auto matroska_result = Media::Matroska::MatroskaDemuxer::from_stream(stream);
        if (!matroska_result.is_error())
            return matroska_result.release_value();
        return Media::FFmpeg::FFmpegDemuxer::from_stream(stream);
    }());
    auto track = TRY_OR_FAIL(demuxer->get_preferred_track_for_type(Media::TrackType::Audio));
    VERIFY(track.has_value());
    auto provider = TRY_OR_FAIL(Media::AudioDataProvider::try_create(Core::EventLoop::current_weak(), demuxer, track.release_value()));

    auto reached_end = false;
    provider->set_error_handler([&](Media::DecoderError&& error) {
        if (error.category() == Media::DecoderErrorCategory::EndOfStream) {
            reached_end = true;
            return;
        }
        FAIL("An error occurred while decoding.");
    });
    provider->start();

    auto time_limit = AK::Duration::from_seconds(1);
    auto start_time = MonotonicTime::now_coarse();

    i64 last_sample = 0;
    size_t sample_count = 0;

    while (true) {
        auto block = provider->retrieve_block();
        if (block.is_empty()) {
            if (reached_end)
                break;
        } else {
            EXPECT_EQ(block.sample_rate(), sample_rate);
            EXPECT_EQ(block.channel_count(), channel_count);
            if (expected_channel_map.has_value())
                EXPECT_EQ(block.sample_specification().channel_map(), expected_channel_map.value());

            VERIFY(sample_count == 0 || last_sample <= block.timestamp_in_samples());
            last_sample = block.timestamp_in_samples() + static_cast<i64>(block.sample_count());

            sample_count += block.sample_count();
        }

        if (MonotonicTime::now_coarse() - start_time >= time_limit) {
            FAIL("Decoding timed out.");
            return;
        }

        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    VERIFY(reached_end);
    EXPECT_EQ(sample_count, expected_sample_count);
}
