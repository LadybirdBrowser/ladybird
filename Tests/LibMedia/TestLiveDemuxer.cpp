/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibAudio/ChannelMap.h>
#include <LibAudio/SampleSpecification.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/LiveDemuxer.h>
#include <LibMedia/MediaTime.h>
#include <LibTest/TestCase.h>

namespace {

constexpr u32 SAMPLE_RATE = 48'000;

struct StoppedClock {
    explicit StoppedClock(AK::Duration time)
        : writer(MUST(Media::MediaTimeWriter::create()))
    {
        writer.publish_monotonic_anchor(MonotonicTime::now(), time, 1.0f, Media::MediaTimeProgression::Stopped);
    }

    Media::MediaTimeReader reader() const { return MUST(Media::MediaTimeReader::create(writer.buffer())); }

    Media::MediaTimeWriter writer;
};

NonnullRefPtr<Media::LiveDemuxer> create_audio_demuxer(StoppedClock const& clock)
{
    auto demuxer = Media::LiveDemuxer::create_for_audio(1, Audio::SampleSpecification(SAMPLE_RATE, Audio::ChannelMap::stereo()));
    demuxer->set_time_reader(clock.reader());
    return demuxer;
}

void push_audio(Media::LiveDemuxer& demuxer, size_t frame_count, float value)
{
    Vector<float> samples;
    samples.resize(frame_count * 2);
    samples.fill(value);
    demuxer.push_audio({ samples.data(), samples.size() * sizeof(float) });
}

float first_sample_of(Media::CodedFrame const& frame)
{
    float sample = 0;
    VERIFY(frame.data().size() >= sizeof(float));
    __builtin_memcpy(&sample, frame.data().data(), sizeof(float));
    return sample;
}

void push_video_frame(Media::LiveDemuxer& demuxer, bool is_key_frame, AK::Duration source_timestamp, AK::Duration live_edge)
{
    Array<u8, 4> data { 1, 2, 3, 4 };
    demuxer.push_video_frame(Media::CodecID::VP8, is_key_frame ? Media::FrameFlags::Keyframe : Media::FrameFlags::None, source_timestamp, MUST(FixedArray<u8>::create(data.span())), live_edge);
}

}

TEST_CASE(audio_is_placed_at_the_playback_position)
{
    Core::EventLoop loop;
    StoppedClock clock { AK::Duration::from_seconds(1) };
    auto demuxer = create_audio_demuxer(clock);
    auto const& track = demuxer->track();

    push_audio(*demuxer, SAMPLE_RATE / 100, 0.5f);

    auto block = TRY_OR_FAIL(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(block.codec_id(), Media::CodecID::F32LE);
    EXPECT_EQ(block.presentation_timestamp(), AK::Duration::from_seconds(1));
    EXPECT_EQ(block.duration(), AK::Duration::from_milliseconds(10));
    EXPECT_EQ(first_sample_of(block), 0.5f);
    // The first block starts a decode sequence.
    EXPECT(block.new_codec_configuration().has_value());
}

TEST_CASE(a_silent_audio_track_keeps_playing)
{
    Core::EventLoop loop;
    StoppedClock clock { AK::Duration::from_seconds(1) };
    auto demuxer = create_audio_demuxer(clock);
    auto const& track = demuxer->track();

    auto silence = TRY_OR_FAIL(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(silence.presentation_timestamp(), AK::Duration::from_seconds(1));
    EXPECT_EQ(first_sample_of(silence), 0.0f);

    push_audio(*demuxer, SAMPLE_RATE / 100, 0.5f);
    auto block = TRY_OR_FAIL(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(block.presentation_timestamp(), AK::Duration::from_milliseconds(1010));
    EXPECT_EQ(first_sample_of(block), 0.5f);
}

TEST_CASE(audio_that_would_play_late_is_dropped)
{
    Core::EventLoop loop;
    StoppedClock clock { AK::Duration::zero() };
    auto demuxer = create_audio_demuxer(clock);

    for (size_t i = 0; i < 100; i++)
        push_audio(*demuxer, SAMPLE_RATE / 100, 0.5f);

    auto end_time = demuxer->audio_end_time();
    EXPECT(end_time > Media::LiveDemuxer::AUDIO_MAXIMUM_LATENCY);
    EXPECT(end_time <= Media::LiveDemuxer::AUDIO_MAXIMUM_LATENCY + AK::Duration::from_milliseconds(10));
}

TEST_CASE(an_ended_audio_track_plays_out_what_it_has)
{
    Core::EventLoop loop;
    StoppedClock clock { AK::Duration::zero() };
    auto demuxer = create_audio_demuxer(clock);
    auto const& track = demuxer->track();

    push_audio(*demuxer, SAMPLE_RATE / 100, 0.5f);
    demuxer->end();
    EXPECT(demuxer->scan_state().reached_end_of_stream);

    auto block = TRY_OR_FAIL(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(first_sample_of(block), 0.5f);
    auto end = demuxer->get_next_sample_for_track(track);
    EXPECT(end.is_error());
    EXPECT_EQ(end.error().category(), Media::DecoderErrorCategory::EndOfStream);
}

TEST_CASE(video_starts_from_a_key_frame)
{
    Core::EventLoop loop;
    auto demuxer = Media::LiveDemuxer::create_for_video(1);
    auto const& track = demuxer->track();
    size_t key_frame_requests = 0;
    demuxer->on_key_frame_needed = [&] { key_frame_requests++; };

    push_video_frame(*demuxer, false, AK::Duration::zero(), AK::Duration::zero());
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(key_frame_requests, 1u);

    push_video_frame(*demuxer, true, AK::Duration::from_milliseconds(33), AK::Duration::from_seconds(2));
    push_video_frame(*demuxer, false, AK::Duration::from_milliseconds(66), AK::Duration::from_seconds(2));
    demuxer->end();

    auto key_frame = TRY_OR_FAIL(demuxer->get_next_sample_for_track(track));
    EXPECT(key_frame.is_keyframe());
    EXPECT_EQ(key_frame.codec_id(), Media::CodecID::VP8);
    EXPECT_EQ(key_frame.presentation_timestamp(), AK::Duration::from_seconds(2));

    // The second frame arrived at the same position, and plays as much later as it was captured.
    auto delta_frame = TRY_OR_FAIL(demuxer->get_next_sample_for_track(track));
    EXPECT(!delta_frame.is_keyframe());
    EXPECT_EQ(delta_frame.presentation_timestamp(), AK::Duration::from_milliseconds(2033));

    auto end = demuxer->get_next_sample_for_track(track);
    EXPECT(end.is_error());
    EXPECT_EQ(end.error().category(), Media::DecoderErrorCategory::EndOfStream);
}

TEST_CASE(video_follows_the_playback_position)
{
    Core::EventLoop loop;
    auto demuxer = Media::LiveDemuxer::create_for_video(1);
    auto const& track = demuxer->track();

    push_video_frame(*demuxer, true, AK::Duration::zero(), AK::Duration::from_seconds(1));
    // A frame that its source's pace would show late plays when it arrives.
    push_video_frame(*demuxer, false, AK::Duration::from_milliseconds(33), AK::Duration::from_seconds(2));
    // So does a frame that the pace would hold back for long, as after the playback was paused.
    push_video_frame(*demuxer, false, AK::Duration::from_seconds(10), AK::Duration::from_seconds(2));

    EXPECT_EQ(TRY_OR_FAIL(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::from_seconds(1));
    EXPECT_EQ(TRY_OR_FAIL(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::from_seconds(2));
    // Frames are decoded in order, so their timestamps always increase.
    EXPECT_EQ(TRY_OR_FAIL(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::from_seconds(2) + AK::Duration::from_microseconds(1));
}

TEST_CASE(video_plays_on_arrival_while_the_playback_stands_still)
{
    Core::EventLoop loop;
    auto demuxer = Media::LiveDemuxer::create_for_video(1);
    auto const& track = demuxer->track();
    Array<u8, 4> data { 1, 2, 3, 4 };
    auto push = [&](Media::FrameFlags flags, AK::Duration source_timestamp) {
        demuxer->push_video_frame(Media::CodecID::VP8, flags, source_timestamp, MUST(FixedArray<u8>::create(data.span())), AK::Duration::from_seconds(1), Media::LiveDemuxer::VideoPacing::OnArrival);
    };

    push(Media::FrameFlags::Keyframe, AK::Duration::zero());
    push(Media::FrameFlags::None, AK::Duration::from_milliseconds(33));

    auto first = TRY_OR_FAIL(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(first.presentation_timestamp(), AK::Duration::from_seconds(1));
    EXPECT_EQ(demuxer->decoder_latency(), Media::DecoderLatency::Low);
    EXPECT_EQ(TRY_OR_FAIL(demuxer->get_next_sample_for_track(track)).presentation_timestamp(), AK::Duration::from_seconds(1) + AK::Duration::from_microseconds(1));
}

TEST_CASE(video_that_is_not_read_needs_a_new_key_frame)
{
    Core::EventLoop loop;
    auto demuxer = Media::LiveDemuxer::create_for_video(1);
    auto const& track = demuxer->track();
    size_t key_frame_requests = 0;
    demuxer->on_key_frame_needed = [&] { key_frame_requests++; };

    push_video_frame(*demuxer, true, AK::Duration::zero(), AK::Duration::zero());
    for (size_t i = 1; i <= Media::LiveDemuxer::MAXIMUM_QUEUED_VIDEO_FRAMES; i++)
        push_video_frame(*demuxer, false, AK::Duration::from_milliseconds(static_cast<i64>(i) * 33), AK::Duration::zero());
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(key_frame_requests, 1u);

    push_video_frame(*demuxer, true, AK::Duration::from_seconds(5), AK::Duration::from_seconds(5));
    auto frame = TRY_OR_FAIL(demuxer->get_next_sample_for_track(track));
    EXPECT(frame.is_keyframe());
    EXPECT_EQ(frame.presentation_timestamp(), AK::Duration::from_seconds(5));
}

TEST_CASE(a_reader_that_starts_over_gets_a_key_frame)
{
    Core::EventLoop loop;
    auto demuxer = Media::LiveDemuxer::create_for_video(1);
    auto const& track = demuxer->track();
    size_t key_frame_requests = 0;
    demuxer->on_key_frame_needed = [&] { key_frame_requests++; };

    push_video_frame(*demuxer, true, AK::Duration::zero(), AK::Duration::zero());
    push_video_frame(*demuxer, false, AK::Duration::from_milliseconds(33), AK::Duration::zero());
    EXPECT(TRY_OR_FAIL(demuxer->get_next_sample_for_track(track)).is_keyframe());

    // The reader's decoder was reset, and what is left depends on the key frame it no longer has.
    auto result = TRY_OR_FAIL(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::zero(), Media::DemuxerSeekOptions::Force));
    EXPECT_EQ(result, Media::DemuxerSeekResult::MovedPosition);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(key_frame_requests, 1u);

    push_video_frame(*demuxer, false, AK::Duration::from_milliseconds(66), AK::Duration::zero());
    push_video_frame(*demuxer, true, AK::Duration::from_milliseconds(99), AK::Duration::zero());
    EXPECT(TRY_OR_FAIL(demuxer->get_next_sample_for_track(track)).is_keyframe());
}
