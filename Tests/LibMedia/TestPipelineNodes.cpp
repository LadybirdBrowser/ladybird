/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/MediaClock.h>
#include <LibMedia/MonotonicMediaClock.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Processors/AudioMixer.h>
#include <LibMedia/Processors/AudioTimeStretchProcessor.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <LibMedia/Sinks/AudioPlaybackSink.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoFramePool.h>
#include <LibTest/TestCase.h>

#include "TestMediaCommon.h"

static NonnullRefPtr<Media::IncrementallyPopulatedStream> load_test_file(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return Media::IncrementallyPopulatedStream::create_from_buffer(MUST(file->read_until_eof()));
}

static NonnullRefPtr<Media::Demuxer> create_demuxer(NonnullRefPtr<Media::IncrementallyPopulatedStream> const& stream)
{
    auto matroska_result = Media::Matroska::MatroskaDemuxer::from_stream(stream);
    if (!matroska_result.is_error())
        return matroska_result.release_value();
    return MUST(Media::FFmpeg::FFmpegDemuxer::from_stream(stream));
}

template<typename Condition>
static bool pump_until(Core::EventLoop& loop, Condition condition)
{
    auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(10);
    while (MonotonicTime::now_coarse() < deadline) {
        if (condition())
            return true;
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
    return false;
}

TEST_CASE(audio_producer_underspecified_5_1_channel_map)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("WAV/tone_44100_5_1_underspecified.wav"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0]));

    producer->start();

    auto time_limit = AK::Duration::from_seconds(1);
    auto start_time = MonotonicTime::now_coarse();

    while (true) {
        auto output = producer->peek();
        if (output.status == Media::PipelineStatus::HaveData) {
            auto const& block = *output.block;
            EXPECT(!block.is_empty());
            EXPECT_EQ(block.channel_count(), 6);
            EXPECT_EQ(block.sample_specification().channel_map(), Audio::ChannelMap::surround_5_1());
            producer->consume();
            return;
        }
        if (MonotonicTime::now_coarse() - start_time >= time_limit)
            break;
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }

    FAIL("Decoding timed out.");
}

TEST_CASE(audio_mixer_seek_during_playback_rewakes_consumer)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("WAV/tone_44100_5_1_underspecified.wav"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0]));

    auto mixer = TRY_OR_FAIL(Media::AudioMixer::try_create());
    TRY_OR_FAIL(mixer->set_output_sample_specification(tracks[0].audio_data().sample_specification));
    TRY_OR_FAIL(mixer->connect_input(producer));

    bool consumer_woken = false;
    mixer->set_wake_handler([&] { consumer_woken = true; });

    mixer->start();

    // Play until the mixer is producing data, mirroring steady playback (its wake-forwarding state is
    // now latched as it would be then).
    EXPECT(pump_until(loop, [&] { return mixer->peek().status == Media::PipelineStatus::HaveData; }));
    mixer->consume();

    // Seek mid-playback. The consumer is now waiting to be woken (it is not polling anymore), so the
    // mixer must forward the producer's post-seek wake downstream or the seek never resolves.
    consumer_woken = false;
    mixer->seek(AK::Duration::zero());

    EXPECT(pump_until(loop, [&] { return consumer_woken; }));
}

TEST_CASE(video_producer_seeks_while_frames_are_held)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("vp9_oob_blocks.webm"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, tracks[0]));
    producer->start();

    auto pull_next_frame = [&]() -> RefPtr<Media::VideoFrame> {
        auto time_limit = AK::Duration::from_seconds(10);
        auto start_time = MonotonicTime::now_coarse();
        while (MonotonicTime::now_coarse() - start_time < time_limit) {
            if (auto output = producer->peek(); output.frame != nullptr) {
                producer->consume();
                return output.frame;
            }
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        }
        return nullptr;
    };

    // Hold a displaying sink's worth of frames plus a lagging media element reference, so the
    // seek below must decode towards its target while these frame pool slots stay occupied.
    Vector<NonnullRefPtr<Media::VideoFrame>> held_frames;
    for (int i = 0; i < 3; i++) {
        auto frame = pull_next_frame();
        if (frame == nullptr)
            FAIL("Timed out pulling the initial frames");
        held_frames.append(frame.release_nonnull());
    }

    // Give the producer time to fill its decode-ahead queue behind the held frames.
    auto fill_until = MonotonicTime::now_coarse() + AK::Duration::from_milliseconds(250);
    while (MonotonicTime::now_coarse() < fill_until)
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    producer->seek(AK::Duration::from_seconds(2));

    if (pull_next_frame() == nullptr)
        FAIL("The seek did not resolve; the producer is likely starved of frame pool slots");
}

TEST_CASE(video_producer_seeks_past_the_end_resolve_within_queued_data)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("vp9_oob_blocks.webm"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, tracks[0]));
    producer->start();

    auto pump_until = [&](auto condition) {
        auto deadline = MonotonicTime::now_coarse() + AK::Duration::from_seconds(5);
        while (MonotonicTime::now_coarse() < deadline) {
            if (condition())
                return true;
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        }
        return false;
    };

    auto past_end = AK::Duration::from_seconds(100);
    producer->seek(past_end);
    EXPECT(pump_until([&] { return producer->peek().status == Media::PipelineStatus::HaveData; }));

    // Give the decode loop time to observe the demuxer's end of stream behind the queued final frame.
    auto fill_until = MonotonicTime::now_coarse() + AK::Duration::from_milliseconds(250);
    while (MonotonicTime::now_coarse() < fill_until)
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);

    // Nothing exists beyond the queued final frame for the demuxer to re-emit, so a further seek past
    // the end must resolve with the queued frame instead of discarding it.
    producer->seek(past_end + AK::Duration::from_seconds(1));
    auto output = producer->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::HaveData);
    EXPECT(output.frame != nullptr);
    producer->consume();
    EXPECT(pump_until([&] { return producer->peek().status == Media::PipelineStatus::EndOfStream; }));

    // With the final frame consumed, nothing downstream is guaranteed to still hold it, so a later seek past the
    // end re-emits that same frame before reporting end of stream again.
    producer->seek(past_end + AK::Duration::from_seconds(2));
    auto re_emitted = producer->peek();
    EXPECT_EQ(re_emitted.status, Media::PipelineStatus::HaveData);
    EXPECT(re_emitted.frame.ptr() == output.frame.ptr());
    producer->consume();
    EXPECT_EQ(producer->peek().status, Media::PipelineStatus::EndOfStream);
}

static constexpr AK::Duration SHORT_AUTO_SUSPEND_TIMEOUT = AK::Duration::from_milliseconds(50);

static void sleep_past_the_idle_timeout()
{
    MUST(Core::System::sleep_ms(static_cast<u32>(SHORT_AUTO_SUSPEND_TIMEOUT.to_milliseconds() * 4)));
}

TEST_CASE(video_producer_auto_suspends_and_resumes_only_from_a_seek)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("big_buck_bunny_5s.webm"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));
    producer->start();

    EXPECT(pump_until(loop, [&] { return producer->peek().status == Media::PipelineStatus::HaveData; }));

    // Left idle, the producer suspends. The checking peeks are spaced wider than the idle timeout
    // so they cannot defer it indefinitely.
    EXPECT(pump_until(loop, [&] {
        sleep_past_the_idle_timeout();
        return producer->peek().status == Media::PipelineStatus::Suspended;
    }));

    // Peeking and consuming must not resume a suspended producer.
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(producer->peek().status, Media::PipelineStatus::Suspended);
        producer->consume();
    }
    sleep_past_the_idle_timeout();
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(producer->peek().status, Media::PipelineStatus::Suspended);

    // A seek resumes decoding and wakes the waiting consumer.
    bool woken = false;
    producer->set_wake_handler([&] { woken = true; });
    producer->seek(AK::Duration::zero());
    EXPECT(pump_until(loop, [&] { return woken && producer->peek().status == Media::PipelineStatus::HaveData; }));
}

TEST_CASE(video_producer_suspends_while_frames_are_held_and_wakes_the_consumer)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("big_buck_bunny_5s.webm"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));
    producer->start();

    // Hold every frame pool slot so the decoder parks waiting for a free slot, as it would behind
    // a paused sink holding frames downstream.
    Vector<NonnullRefPtr<Media::VideoFrame>> held_frames;
    EXPECT(pump_until(loop, [&] {
        auto output = producer->peek();
        if (output.frame != nullptr) {
            held_frames.append(output.frame.release_nonnull());
            producer->consume();
        }
        return held_frames.size() == Media::VideoFramePool::MAX_SLOT_COUNT;
    }));

    // The queue is drained, so the last observed status is a waiting one and entering suspension
    // must dispatch a wake.
    EXPECT_EQ(producer->peek().status, Media::PipelineStatus::Pending);
    // Run any wake dispatch still queued from draining the frames above, so the wait below only
    // observes the wake dispatched by entering suspension.
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    bool woken = false;
    producer->set_wake_handler([&] { woken = true; });
    EXPECT(pump_until(loop, [&] { return woken; }));
    EXPECT_EQ(producer->peek().status, Media::PipelineStatus::Suspended);
}

TEST_CASE(audio_producer_auto_suspends_and_resumes_only_from_a_seek)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("WAV/tone_44100_stereo.wav"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));
    producer->start();

    EXPECT(pump_until(loop, [&] { return producer->peek().status == Media::PipelineStatus::HaveData; }));

    EXPECT(pump_until(loop, [&] {
        sleep_past_the_idle_timeout();
        return producer->peek().status == Media::PipelineStatus::Suspended;
    }));

    // A seek resumes decoding and wakes the waiting consumer.
    bool woken = false;
    producer->set_wake_handler([&] { woken = true; });
    producer->seek(AK::Duration::zero());
    EXPECT(pump_until(loop, [&] { return woken && producer->peek().status == Media::PipelineStatus::HaveData; }));
}

TEST_CASE(audio_mixer_forwards_a_suspended_input)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("WAV/tone_44100_stereo.wav"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));

    auto mixer = TRY_OR_FAIL(Media::AudioMixer::try_create());
    TRY_OR_FAIL(mixer->set_output_sample_specification(tracks[0].audio_data().sample_specification));
    TRY_OR_FAIL(mixer->connect_input(producer));
    mixer->start();

    // Leave the output unconsumed, so that the suspension arrives while data is buffered.
    EXPECT(pump_until(loop, [&] { return mixer->peek().status == Media::PipelineStatus::HaveData; }));

    EXPECT(pump_until(loop, [&] {
        sleep_past_the_idle_timeout();
        return producer->peek().status == Media::PipelineStatus::Suspended;
    }));

    // The mixer drops what it holds and reports the suspension, leaving the resume to its consumer's seek.
    auto output = mixer->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::Suspended);
    EXPECT(output.block == nullptr);

    mixer->seek(AK::Duration::zero());
    EXPECT(pump_until(loop, [&] { return mixer->peek().status == Media::PipelineStatus::HaveData; }));
}

TEST_CASE(audio_time_stretch_processor_forwards_a_suspended_input)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("WAV/tone_44100_stereo.wav"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));

    auto stretcher = TRY_OR_FAIL(Media::AudioTimeStretchProcessor::try_create());
    TRY_OR_FAIL(stretcher->set_output_sample_specification(tracks[0].audio_data().sample_specification));
    TRY_OR_FAIL(stretcher->connect_input(producer));
    stretcher->start();

    // Leave the output unconsumed, so that the suspension arrives while data is buffered.
    EXPECT(pump_until(loop, [&] { return stretcher->peek().status == Media::PipelineStatus::HaveData; }));

    EXPECT(pump_until(loop, [&] {
        sleep_past_the_idle_timeout();
        return producer->peek().status == Media::PipelineStatus::Suspended;
    }));

    // The stretcher drops what it holds and reports the suspension, leaving the resume to its consumer's seek.
    auto output = stretcher->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::Suspended);
    EXPECT(output.block == nullptr);

    stretcher->seek(AK::Duration::zero());
    EXPECT(pump_until(loop, [&] { return stretcher->peek().status == Media::PipelineStatus::HaveData; }));
}

// The output frame is numbered apart from the media time once it ran at a rate other than 1, so a seek that continues an
// already written output must keep numbering from that output.
TEST_CASE(audio_time_stretch_processor_continues_output_at_the_requested_frame)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("WAV/tone_44100_stereo.wav"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    VERIFY(!tracks.is_empty());
    auto producer = TRY_OR_FAIL(Media::DecodedAudioProducer::try_create(loop, demuxer, tracks[0]));

    auto stretcher = TRY_OR_FAIL(Media::AudioTimeStretchProcessor::try_create());
    TRY_OR_FAIL(stretcher->set_output_sample_specification(tracks[0].audio_data().sample_specification));
    TRY_OR_FAIL(stretcher->connect_input(producer));
    stretcher->set_playback_rate(2.0f);
    stretcher->start();

    auto media_target = AK::Duration::from_seconds(1);
    i64 output_frame = 10'000;
    stretcher->seek_continuing_at_output_frame(media_target, output_frame);

    Optional<Media::AudioBlockTiming> timing_at_output_frame;
    EXPECT(pump_until(loop, [&] {
        auto output = stretcher->peek();
        if (output.status != Media::PipelineStatus::HaveData)
            return false;
        auto timing = output.block->timing();
        stretcher->consume();
        if (timing.end_frame_index() <= output_frame)
            return false;
        timing_at_output_frame = timing;
        return true;
    }));
    VERIFY(timing_at_output_frame.has_value());
    EXPECT(timing_at_output_frame->contains_frame_index(output_frame));
    auto media_time_at_output_frame = timing_at_output_frame->media_time_at_frame_index(output_frame);
    EXPECT(media_time_at_output_frame > media_target - AK::Duration::from_milliseconds(2));
    EXPECT(media_time_at_output_frame < media_target + AK::Duration::from_milliseconds(2));
}

namespace {

// Produces silent blocks endlessly, with media time advancing at twice the rate of the output frames as a time
// stretcher at a rate of 2 would number them, and records the seeks it receives.
class RecordingAudioProducer final : public Media::AudioProducer {
public:
    struct Seek {
        AK::Duration timestamp;
        Optional<i64> output_frame;
    };

    static NonnullRefPtr<RecordingAudioProducer> create() { return adopt_ref(*new RecordingAudioProducer()); }

    virtual void start() override { }
    virtual ErrorOr<void> set_output_sample_specification(Audio::SampleSpecification sample_specification) override
    {
        MutexLocker locker { m_mutex };
        m_sample_specification = sample_specification;
        return {};
    }

    virtual Media::AudioProducerOutput peek() override
    {
        MutexLocker locker { m_mutex };
        if (!m_sample_specification.is_valid())
            return { nullptr, Media::PipelineStatus::Pending };
        if (m_block.is_empty()) {
            m_block.initialize(m_sample_specification, m_next_output_frame, frames_per_block);
            for (size_t channel = 0; channel < m_block.channel_count(); channel++)
                m_block.channel_data(channel).fill(0.0f);
            m_block.set_media_time_start(m_next_media_time);
            m_block.set_media_time_duration(media_time_per_block());
        }
        return { &m_block, Media::PipelineStatus::HaveData };
    }

    virtual void consume() override
    {
        MutexLocker locker { m_mutex };
        m_next_output_frame += frames_per_block;
        m_next_media_time += media_time_per_block();
        m_block.clear();
    }

    virtual void set_wake_handler(Media::PipelineWakeHandler handler) override { m_wake_handler = move(handler); }

    virtual void seek(AK::Duration timestamp) override
    {
        reposition(timestamp, timestamp.to_time_units(1, output_sample_rate()), {});
    }

    virtual void seek_continuing_at_output_frame(AK::Duration timestamp, i64 output_frame) override
    {
        reposition(timestamp, output_frame, output_frame);
    }

    Vector<Seek> seeks() const
    {
        MutexLocker locker { m_mutex };
        return m_seeks;
    }

    u32 output_sample_rate() const
    {
        MutexLocker locker { m_mutex };
        return m_sample_specification.sample_rate();
    }

private:
    static constexpr size_t frames_per_block = 1024;

    RecordingAudioProducer() = default;

    AK::Duration media_time_per_block() const { return AK::Duration::from_time_units(frames_per_block * 2, 1, m_sample_specification.sample_rate()); }

    void reposition(AK::Duration timestamp, i64 output_frame, Optional<i64> recorded_output_frame)
    {
        {
            MutexLocker locker { m_mutex };
            m_seeks.append({ timestamp, recorded_output_frame });
            m_next_output_frame = output_frame;
            m_next_media_time = timestamp;
            m_block.clear();
        }
        if (m_wake_handler)
            m_wake_handler();
    }

    mutable Mutex m_mutex;
    Audio::SampleSpecification m_sample_specification;
    Media::AudioBlock m_block;
    i64 m_next_output_frame { 0 };
    AK::Duration m_next_media_time;
    Vector<Seek> m_seeks;
    Media::PipelineWakeHandler m_wake_handler;
};

}

TEST_CASE(audio_playback_sink_reseeks_its_input_from_the_written_output_without_moving_the_clock)
{
    auto& loop = never_destroyed_event_loop();

    auto sink = MUST(Media::AudioPlaybackSink::try_create([](Media::PipelineStatus) { }, Media::AudioOutput::Null));
    auto producer = RecordingAudioProducer::create();
    MUST(sink->connect_input(producer));
    sink->start();
    EXPECT(pump_until(loop, [&] { return !producer->seeks().is_empty(); }));

    sink->resume();
    auto time_reader = sink->time_reader();
    EXPECT(pump_until(loop, [&] { return time_reader.current_time() > AK::Duration::from_milliseconds(300); }));

    auto time_before_reseeking = time_reader.current_time();
    sink->reseek_input_keeping_clock();
    auto time_after_reseeking = time_reader.current_time();

    // The input continues the written output, at the media time that the written blocks reached by that frame.
    auto seeks = producer->seeks();
    auto const& reseek = seeks.last();
    EXPECT(reseek.output_frame.has_value());
    if (!reseek.output_frame.has_value())
        return;
    EXPECT(reseek.output_frame.value() > 0);
    auto sample_rate = producer->output_sample_rate();
    auto expected_timestamp = AK::Duration::from_time_units(reseek.output_frame.value() * 2, 1, sample_rate);
    EXPECT(reseek.timestamp > expected_timestamp - AK::Duration::from_milliseconds(1));
    EXPECT(reseek.timestamp < expected_timestamp + AK::Duration::from_milliseconds(1));

    EXPECT(time_after_reseeking >= time_before_reseeking);
    EXPECT(time_after_reseeking - time_before_reseeking < AK::Duration::from_milliseconds(50));
    EXPECT(pump_until(loop, [&] { return time_reader.current_time() > time_after_reseeking + AK::Duration::from_milliseconds(200); }));
}

TEST_CASE(displaying_video_sink_reports_a_suspended_input_while_unticked)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("big_buck_bunny_5s.webm"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));

    auto clock = TRY_OR_FAIL(Media::MonotonicMediaClock::try_create());
    auto sink = TRY_OR_FAIL(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    Optional<Media::PipelineStatus> last_dispatched_status;
    sink->set_state_change_handler([&](Media::PipelineStatus status) { last_dispatched_status = status; });
    TRY_OR_FAIL(sink->connect_input(producer));

    EXPECT(pump_until(loop, [&] {
        (void)sink->update(MonotonicTime::now());
        return sink->current_frame() != nullptr;
    }));

    // With no further ticks, the suspension wake is the sink's only signal, and it must forward
    // the status to its user.
    EXPECT(pump_until(loop, [&] { return last_dispatched_status == Media::PipelineStatus::Suspended; }));
}

TEST_CASE(displaying_video_sink_demand_seeks_a_suspended_input)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = load_test_file("big_buck_bunny_5s.webm"sv);
    auto demuxer = create_demuxer(stream);
    auto tracks = TRY_OR_FAIL(demuxer->get_tracks_for_type(Media::TrackType::Video));
    VERIFY(!tracks.is_empty());

    auto producer = TRY_OR_FAIL(Media::DecodedVideoProducer::try_create(loop, demuxer, tracks[0], SHORT_AUTO_SUSPEND_TIMEOUT));

    auto clock = TRY_OR_FAIL(Media::MonotonicMediaClock::try_create());
    auto sink = TRY_OR_FAIL(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    TRY_OR_FAIL(sink->connect_input(producer));

    // Display the first frame, then leave the sink unticked until the producer suspends.
    EXPECT(pump_until(loop, [&] {
        (void)sink->update(MonotonicTime::now());
        return sink->current_frame() != nullptr;
    }));
    EXPECT(pump_until(loop, [&] {
        sleep_past_the_idle_timeout();
        return producer->peek().status == Media::PipelineStatus::Suspended;
    }));

    // Ticks while the displayed frame still covers the paused clock leave the input suspended.
    (void)sink->update(MonotonicTime::now());
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(producer->peek().status, Media::PipelineStatus::Suspended);

    // Seeking past the cached frames seeks the input, resuming decoding at the new position.
    auto initial_frame = sink->current_frame();
    clock->seek(AK::Duration::from_seconds(1));
    sink->seek(AK::Duration::from_seconds(1));
    EXPECT(pump_until(loop, [&] {
        (void)sink->update(MonotonicTime::now());
        return sink->current_frame() != nullptr && sink->current_frame() != initial_frame;
    }));
    EXPECT(sink->current_frame()->timestamp() > initial_frame->timestamp());
}
