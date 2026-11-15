/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/DecoderRegistrations.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/MediaClock.h>
#include <LibMedia/MonotonicMediaClock.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Processors/AudioMixer.h>
#include <LibMedia/Processors/AudioTimeStretchProcessor.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <LibMedia/Sinks/AudioOutputFactory.h>
#include <LibMedia/Sinks/AudioPlaybackSink.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibTest/TestCase.h>

#include "TestMediaCommon.h"

TEST_SETUP
{
    Media::install_video_decoders({ Media::hardware_video_decoders(), Media::ffmpeg_video_decoders() });
    Media::install_audio_decoders({ Media::audio_decoders() });
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

template<typename Node>
static bool seek_to_resume_and_pump_until_data(Core::EventLoop& loop, Node& node, AK::Duration timestamp)
{
    node.seek(timestamp);
    return pump_until(loop, [&] {
        auto status = node.peek().status;
        if (status == Media::PipelineStatus::Suspended) {
            node.seek(timestamp);
            return false;
        }
        return status == Media::PipelineStatus::HaveData;
    });
}

// The producer may suspend before the sink takes a frame, and the sink only resumes it on demand.
static bool pump_until_the_sink_displays_a_frame(Core::EventLoop& loop, Media::DisplayingVideoSink& sink, Media::DecodedVideoProducer& producer, Media::MediaClock const& clock)
{
    return pump_until(loop, [&] {
        (void)sink.update(MonotonicTime::now());
        if (sink.current_frame() != nullptr)
            return true;
        if (producer.peek().status == Media::PipelineStatus::Suspended)
            sink.seek(clock.time_reader().current_time());
        return false;
    });
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
    EXPECT(seek_to_resume_and_pump_until_data(loop, *mixer, AK::Duration::zero()));

    EXPECT(pump_until(loop, [&] {
        sleep_past_the_idle_timeout();
        return producer->peek().status == Media::PipelineStatus::Suspended;
    }));

    // The mixer drops what it holds and reports the suspension, leaving the resume to its consumer's seek.
    auto output = mixer->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::Suspended);
    EXPECT(output.block == nullptr);

    EXPECT(seek_to_resume_and_pump_until_data(loop, *mixer, AK::Duration::zero()));
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
    EXPECT(seek_to_resume_and_pump_until_data(loop, *stretcher, AK::Duration::zero()));

    EXPECT(pump_until(loop, [&] {
        sleep_past_the_idle_timeout();
        return producer->peek().status == Media::PipelineStatus::Suspended;
    }));

    // The stretcher drops what it holds and reports the suspension, leaving the resume to its consumer's seek.
    auto output = stretcher->peek();
    EXPECT_EQ(output.status, Media::PipelineStatus::Suspended);
    EXPECT(output.block == nullptr);

    EXPECT(seek_to_resume_and_pump_until_data(loop, *stretcher, AK::Duration::zero()));
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

    auto sink = MUST(Media::AudioPlaybackSink::try_create([](Media::PipelineStatus) { }, Media::AudioOutputFactory::create_null()));
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

// Video sinks stop watching a stopped clock until they are told it resumed, so a seek while playing must hold the clock
// rather than stop it. Pausing must then stop it, even before the stream has finished stopping for the seek.
TEST_CASE(audio_playback_sink_holds_its_clock_through_a_seek_while_playing)
{
    auto& loop = never_destroyed_event_loop();

    auto sink = MUST(Media::AudioPlaybackSink::try_create([](Media::PipelineStatus) { }, Media::AudioOutputFactory::create_null()));
    auto producer = RecordingAudioProducer::create();
    MUST(sink->connect_input(producer));
    sink->start();
    EXPECT(pump_until(loop, [&] { return !producer->seeks().is_empty(); }));

    sink->resume();
    auto time_reader = sink->time_reader();
    EXPECT(pump_until(loop, [&] { return time_reader.current_time() > AK::Duration::from_milliseconds(100); }));

    sink->seek(AK::Duration::from_seconds(2));
    EXPECT(time_reader.time_state(MonotonicTime::now()).is_advancing);
    EXPECT(pump_until(loop, [&] { return time_reader.current_time() > AK::Duration::from_milliseconds(2100); }));

    sink->seek(AK::Duration::from_seconds(1));
    sink->pause();
    EXPECT(!time_reader.time_state(MonotonicTime::now()).is_advancing);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(pump_until(loop, [&] { return !time_reader.time_state(MonotonicTime::now()).is_advancing; }));
}

TEST_CASE(audio_playback_sink_reports_readiness_at_zero_rate)
{
    auto& loop = never_destroyed_event_loop();

    Optional<Media::PipelineStatus> last_status;
    auto sink = MUST(Media::AudioPlaybackSink::try_create([&](Media::PipelineStatus status) { last_status = status; }, Media::AudioOutputFactory::create_null()));
    auto producer = RecordingAudioProducer::create();
    MUST(sink->connect_input(producer));
    sink->set_playback_rate(0.0f);
    sink->start();
    sink->resume();

    // Readiness must resolve while playback is stopped, without waiting for a producer's idle timeout.
    if (!pump_until(loop, [&] { return last_status == Media::PipelineStatus::HaveData; })) {
        FAIL("The sink did not report ready data at a playback rate of zero");
        return;
    }
    auto time_reader = sink->time_reader();
    EXPECT_EQ(time_reader.current_time(), AK::Duration::zero());
    EXPECT(!time_reader.time_state(MonotonicTime::now()).is_advancing);

    // Seeking must also resolve while the clock remains stopped at the new position.
    last_status.clear();
    auto seek_target = AK::Duration::from_seconds(2);
    sink->seek(seek_target);
    if (!pump_until(loop, [&] { return last_status == Media::PipelineStatus::HaveData; })) {
        FAIL("The sink did not report ready data after seeking at a playback rate of zero");
        return;
    }
    EXPECT_EQ(time_reader.current_time(), seek_target);
    EXPECT(!time_reader.time_state(MonotonicTime::now()).is_advancing);

    sink->set_playback_rate(1.0f);
    EXPECT(pump_until(loop, [&] { return time_reader.current_time() > seek_target + AK::Duration::from_milliseconds(100); }));
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

    EXPECT(pump_until_the_sink_displays_a_frame(loop, *sink, *producer, *clock));

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
    EXPECT(pump_until_the_sink_displays_a_frame(loop, *sink, *producer, *clock));
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
